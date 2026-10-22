/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   This file provides code for the configuration
  *          of the RTC instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */
#include "power_policy.h"

/* Запуск часового кварца и откат на LSI (A5).
 *
 * Раньше LSE поднимала SystemClock_Config — при старте и на КАЖДОМ
 * пробуждении, — а отказ кварца уводил в Error_Handler: перезагрузка сторожем
 * через ~30 с, и снова то же. До записи строки дело не доходило, так что
 * колонка lse_ok = 0, заведённая ровно для этого случая, записаться не могла;
 * в архиве — ноль строк, батарея садится втрое быстрее.
 *
 * Теперь кварц запускается один раз здесь, с таймаутом LSE_STARTUP_TIMEOUT.
 * Не завёлся — RTC идёт от LSI (17–47 кГц: время уплывает, но прибор
 * просыпается и пишет), а в каждой строке lse_ok = 0. Смена источника RTC
 * сбрасывает backup-домен вместе с календарём — это честно: время после
 * отказа кварца и так недостоверно, и time_service это увидит. */
static uint8_t rtc_lse_ok = 0;

/* Окно проверки «RTC идёт» (S1): SSR при PREDIV_S = 255 меняется каждые
 * ~3.9 мс, так что за ~50 мс живой кварц даёт десяток смен. Опрос — счётом:
 * каждая итерация — два чтения шины APB1, то есть не меньше 4 тактов, и
 * SystemCoreClock / 80 итераций не короче 50 мс. */
#define RTC_TICK_CHECK_POLLS  (SystemCoreClock / 80U)

static uint32_t RTC_ReadSsr(void *ctx)
{
  (void)ctx;
  uint32_t ssr = RTC->SSR;
  /* Чтение SSR защёлкивает теневые TR/DR до чтения DR — отпускаем сразу,
   * чтобы защёлка не мешала следующим чтениям (и HAL_RTC_GetTime потом). */
  (void)RTC->DR;
  return ssr;
}

/* Перевод RTC на LSI: кварц не завёлся при старте или встал после (S1).
 * LSEON снимаем прямо в регистре, а не через HAL_RCC_OscConfig: тот после
 * снятия ждёт, пока погаснет LSERDY, а у вставшего кварца LSERDY может
 * висеть — это ещё до 10 с ожидания без пинка. Смена RTCSEL в MspInit
 * сбросит backup-домен целиком, вместе с LSERDY. */
static void RTC_UseLsi(void)
{
  RCC_OscInitTypeDef osc = {0};

  rtc_lse_ok = 0;
  HAL_PWR_EnableBkUpAccess();
  RCC->BDCR &= ~RCC_BDCR_LSEON;
  osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
  osc.LSIState = RCC_LSI_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  (void)HAL_RCC_OscConfig(&osc);
}

static void RTC_StartClockSource(void)
{
  RCC_OscInitTypeDef osc = {0};

  osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
  osc.LSEState = RCC_LSE_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  /* Ожидание LSE — до 10 с, а при переходе LSI → LSE HAL после сброса
   * backup-домена ждёт его ещё раз столько же: вместе это у самого края
   * худшего периода сторожа (~22 с). Пинок между ожиданиями — напрямую,
   * как в обработчиках сбоев. */
  IWDG->KR = 0x0000AAAAU;
  HAL_StatusTypeDef st = HAL_RCC_OscConfig(&osc);
  IWDG->KR = 0x0000AAAAU;

  /* LSERDY ещё не значит, что кварц идёт (S1). Если LSEON уже стоял (RTC
   * работал от LSE до сброса), HAL только выставляет бит и сразу видит
   * старый LSERDY: остановку LSE F401 не детектирует, и флаг не гаснет.
   * Дальше SetWakeUpTimer ждал бы такта RTC, которого нет, — Error_Handler,
   * сторож, и снова то же: цикл перезагрузок без единой строки. Поэтому,
   * если RTC уже идёт от LSE, проверяем по SSR, что он действительно идёт. */
  if (st == HAL_OK &&
      (RCC->BDCR & (RCC_BDCR_RTCEN | RCC_BDCR_RTCSEL)) == (RCC_BDCR_RTCEN | RCC_BDCR_RTCSEL_0) &&
      !Policy_CounterMoves(RTC_ReadSsr, NULL, RTC_TICK_CHECK_POLLS))
  {
    st = HAL_TIMEOUT;
  }

  if (st == HAL_OK)
  {
    rtc_lse_ok = 1;
    return;
  }

  /* Глушим недозапустившийся генератор — он продолжал бы потреблять — и
   * поднимаем LSI. Он нужен и сторожу, так что отдельной цены у него нет. */
  RTC_UseLsi();
}

/* Взвести таймер пробуждения на (wut + 1) с. Если RTC не отвечает (кварц
 * встал уже после проверки при старте), HAL ждёт флага WUTWF, которому
 * нужны такты RTC, и возвращает ошибку. Раньше она вела в Error_Handler —
 * и снова в цикл перезагрузок (S1). Теперь RTC переводится на LSI и таймер
 * взводится ещё раз; отказ и на LSI — это уже не кварц, отдаём вызывающему.
 * Только для старта: здесь сброс backup-домена случается до того, как main
 * прочтёт календарь и счётчик стартов. Перевзвод перед сном в main.c при
 * отказе перезапускает плату, и откат идёт этим же путём. */
static HAL_StatusTypeDef RTC_ArmWakeup(uint32_t wut)
{
  (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
  if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, wut, RTC_WAKEUPCLOCK_CK_SPRE_16BITS) == HAL_OK)
    return HAL_OK;
  if (!rtc_lse_ok)
    return HAL_ERROR;

  /* Повторная инициализация с состояния RESET зовёт MspInit, а тот при
   * rtc_lse_ok = 0 выбирает LSI — со сбросом backup-домена: календарь и DR1/DR2
   * пропадают, time_service увидит это как потерю часов (time_ok = 0). */
  IWDG->KR = 0x0000AAAAU;
  RTC_UseLsi();
  hrtc.State = HAL_RTC_STATE_RESET;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
    return HAL_ERROR;
  (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
  return HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, wut, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
}

uint8_t RTC_LseOk(void)
{
  return rtc_lse_ok;
}

/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */
  RTC_StartClockSource();
  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
//  sTime.Hours = 0x0;
//  sTime.Minutes = 0x0;
//  sTime.Seconds = 0x0;
//  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
//  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
//  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
//  {
//    Error_Handler();
//  }
//  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
//  sDate.Month = RTC_MONTH_JANUARY;
//  sDate.Date = 0x1;
//  sDate.Year = 0x0;
//
//  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
//  {
//    Error_Handler();
//  }
  /* USER CODE BEGIN RTC_Init 2 */
  // LSE 32.768 кГц, предделители уже настроены -> RTC_WAKEUPCLOCK_CK_SPRE_16BITS даёт шаг 1 сек.
  // На LSI (кварц не завёлся) тот же шаг — «секунда» LSI, то есть 0.7–1.9 с.
  // Интервал замеров — RTC_WAKEUP_INTERVAL_SEC в rtc.h (3600 с, полевое значение);
  // сон режется на куски, и main.c перевзводит таймер на кусок перед первым сном.
  if (RTC_ArmWakeup(RTC_WAKEUP_INTERVAL_SEC) != HAL_OK)
  {
      Error_Handler();
  }
  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */
    /* Кварц не завёлся — сгенерированный ниже выбор LSE ушёл бы в таймаут и
     * в Error_Handler. Выбираем LSI сами и выходим, минуя его. */
    if (!rtc_lse_ok)
    {
      PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
      PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
      if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
      {
        Error_Handler();
      }
      __HAL_RCC_RTC_ENABLE();
      HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 0, 0);
      HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
      return;
    }
  /* USER CODE END RTC_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();
  /* USER CODE BEGIN RTC_MspInit 1 */
    HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */

  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();
  /* USER CODE BEGIN RTC_MspDeInit 1 */

  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
/* Сон режется на куски для сторожа (A4), и после каждого пробуждения надо
 * знать, разбудил ли нас таймер (кусок отработан) или что-то другое. */
static volatile uint8_t rtc_wakeup_fired = 0;

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc)
{
    (void)hrtc;
    rtc_wakeup_fired = 1;
}

uint8_t RTC_TakeWakeup(void)
{
    uint8_t f = rtc_wakeup_fired;
    rtc_wakeup_fired = 0;
    return f;
}
/* USER CODE END 1 */

