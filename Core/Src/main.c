/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "adc.h"
#include "fatfs.h"
#include "i2c.h"
#include "rtc.h"
#include "spi.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include "aht20.h"
#include "spiflash.h"
#include "time_service.h"
#include "soil_sensor.h"
#include "ds18b20.h"
#include "usbd_core.h"
#include "usbd_def.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
extern USBD_HandleTypeDef hUsbDeviceFS;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* TEST-ONLY: deliberate hold-awake window after each sample, so STOP-mode current
 * is easy to see as a clean step on an ammeter/scope, and to leave a window for
 * ST-Link access without needing a reset-based connect. Remove/shrink for production. */
#define TEST_AWAKE_HOLD_MS   10000U

/* Время на стабилизацию питания датчиков (AHT20/DS18B20/почва) после включения PA0. */
#define SENSOR_POWERUP_DELAY_MS   500U

/* Пока кнопка удерживается, МК не уходит в STOP — так USB-сессия не рвётся на время сна.
 * BTN_HOLD_MAX_MS — страховка на случай залипшей/залитой кнопки в поле: по истечении
 * этого времени засыпаем принудительно, иначе устройство молча высадит батарею. */
#define BTN_HOLD_POLL_MS   100U
#define BTN_HOLD_MAX_MS    300000U   /* 5 минут */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
FRESULT res;
static BYTE work[4096];
FIL file;
static uint8_t lse_ok = 0;   /* запустился ли LSE-кварц (иначе метки времени врут) */
static volatile uint8_t woke_by_button = 0;  /* выставляется в EXTI-колбэке кнопки */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void Log(const char *msg)
{
  // если позже добавишь UART — заменишь на HAL_UART_Transmit
  // пока можно оставить пустым или использовать semihosting
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // разрешить трассировочный блок ядра
    DWT->CYCCNT = 0;                                 // сбросить счётчик тактов
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;             // запустить счётчик тактов
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
//  HAL_Init();
//  DWT->CTRL |= 1;
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_FATFS_Init();
  MX_USB_DEVICE_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_RTC_Init();
  // LSE НЕ запустился → RTC не на кварце → метки времени будут неправильными.
  // Раньше это "помечалось" записью в PA0, но там теперь вход кнопки — пишем флаг в CSV.
  lse_ok = (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) != RESET) ? 1U : 0U;
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */

  // Включаем питание датчиков (LOW)
  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);

  // === 0) Один раз выставляем время, если календарь ещё не инициализирован ===
  if (__HAL_RTC_IS_CALENDAR_INITIALIZED(&hrtc) == 0U)
  {
      Time_InitOnce();
  }

  // 1) Инициализация SPI флеш
  SPI_Flash_Init();

  f_mkfs(USERPath, FM_FAT, 0, work, sizeof(work));
  f_mount(&USERFatFS, USERPath, 1);

  // 2) Монтируем диск
  res = f_mount(&USERFatFS, USERPath, 1);
  if (res == FR_NO_FILESYSTEM)
  {
      res = f_mkfs(USERPath, FM_FAT, 0, work, sizeof(work));
      if (res == FR_OK)
          res = f_mount(&USERFatFS, USERPath, 1);
  }

  if (res != FR_OK)
  {
      while (1); // критическая ошибка
  }

  f_unlink("data.csv");

  // 3) Создаём CSV файл с заголовком, если файла нет
  res = f_open(&file, "data.csv", FA_OPEN_EXISTING | FA_WRITE);

  if (res == FR_NO_FILE)
  {
      // Файл отсутствует → создаём новый и пишем заголовок
      res = f_open(&file, "data.csv", FA_OPEN_APPEND | FA_WRITE);
      if (res == FR_OK)
      {
          f_puts("timestamp,air_temp,air_hum,soil_temp,soil_hum,usb_state,aht_init_st,aht_read_st,aht_i2c_err,aht_ready_ms,t_sensors_ms,t_write_ms,aht_status,btn,lse_ok,ds_err,wake_src,hold_ms\r\n", &file);
          f_close(&file);
      }
  }
  else if (res == FR_OK)
  {
      // Файл существует → просто закрываем
      f_close(&file);
  }

  // === Первое чтение и запись при старте ===
  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);
  HAL_Delay(200); // дать датчикам стабилизироваться

  AHT20_Data aht;
  AHT20_ReadData(&hi2c1, &aht);

  float soil_temp;
  if (!DS18B20_ReadTemp(GPIOA, GPIO_PIN_8, &soil_temp))
        soil_temp = NAN; // датчик не ответил или CRC не сошёлся

  float soil_hum  = Soil_ReadMoisture();

  char ts[32];
  Time_GetTimestamp(ts, sizeof(ts));

//  res = f_open(&file, "data.csv", FA_OPEN_EXISTING | FA_WRITE);
//  if (res == FR_OK)
//  {
//      f_lseek(&file, f_size(&file));
//
//      char line[128];
//      snprintf(line, sizeof(line),
//               "%s,%.2f,%.2f,%.2f,%.2f\r\n",
//               ts,
//               aht.temperature,
//               aht.humidity,
//               soil_temp,
//               soil_hum);
//
//      f_puts(line, &file);
//      f_close(&file);
//  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint8_t usb_mode = 0;
  uint32_t last_write_ms = 0;   /* TEST-ONLY: длительность записи в флеш прошлого цикла */
  uint32_t last_hold_ms  = 0;   /* сколько мс держали кнопку перед прошлым уходом в сон */
  while (1)
  {
	  /* --- Пока кнопка удерживается, в сон не уходим ---
	   * Это режим "живого" USB: пользователь держит KEY, пока хост читает файл,
	   * и устройство не пропадает с шины на время STOP. Датчики при этом уже
	   * обесточены в конце прошлой итерации, так что лишнего тока не тратим. */
	  uint32_t t_hold0 = HAL_GetTick();
	  last_hold_ms = 0;
	  while (HAL_GPIO_ReadPin(WAKE_BTN_GPIO_Port, WAKE_BTN_Pin) == GPIO_PIN_RESET)
	  {
	      last_hold_ms = HAL_GetTick() - t_hold0;
	      if (last_hold_ms >= BTN_HOLD_MAX_MS)
	          break;   /* кнопка залипла — засыпаем, чтобы не высадить батарею */

	      HAL_Delay(BTN_HOLD_POLL_MS);
	  }

	  /* --- уход в сон: STOP mode, будим только по RTC Wakeup Timer ---
	   * Таймер перевзводим заново перед каждым входом в STOP, чтобы длительность сна
	   * была стабильной (RTC_WAKEUP_INTERVAL_SEC) каждый цикл, а не "плавала" от фазы
	   * свободно бегущего таймера относительно переменной длины активной фазы. */
	  HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
	  HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, RTC_WAKEUP_INTERVAL_SEC, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);

	  woke_by_button = 0;   /* сбрасываем перед сном: интересен источник ЭТОГО пробуждения */

	  HAL_SuspendTick();
	  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

	  /* --- пробуждение: ядро на HSI без PLL, обязательно поднять клоки прежде чем
	   * трогать что-либо ещё (I2C/SPI/ADC/DWT-задержки зависят от реальной частоты) --- */
	  SystemClock_Config();
	  HAL_ResumeTick();

	  uint8_t wake_src = woke_by_button;   /* 1 = кнопка, 0 = плановое пробуждение по RTC */

	  uint32_t t_wake = HAL_GetTick();   /* TEST-ONLY: засекаем длительность активной фазы */

	  /* --- включаем датчики (LOW) --- */
	  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);
	  HAL_Delay(50);   /* минимальная пауза на нарастание питания перед опросом шины */

	  /* Датчик на шине I2C1 только что был полностью обесточен. Жёстко сбрасываем саму
	   * периферию I2C1 через RCC перед повторным использованием — иначе она может остаться
	   * в залипшем BUSY-состоянии после неудачной транзакции прошлого цикла и больше
	   * никогда не восстановиться сама (обычный DeInit/Init этого не гарантирует). */
	  HAL_I2C_DeInit(&hi2c1);
	  __HAL_RCC_I2C1_FORCE_RESET();
	  __HAL_RCC_I2C1_RELEASE_RESET();
	  MX_I2C1_Init();

	  /* Ждём, пока AHT20 реально начнёт подтверждать свой адрес на шине, вместо слепой
	   * фиксированной паузы. aht_ready_ms — сколько мс от подачи питания это заняло
	   * (9999 = так и не ответил за отведённое время). */
	  uint32_t t_power_on = HAL_GetTick();
	  uint16_t aht_ready_ms = 9999;
	  for (uint16_t i = 0; i < 80; i++)   /* до ~2 секунд ожидания */
	  {
	      if (HAL_I2C_IsDeviceReady(&hi2c1, AHT20_ADDR, 1, 10) == HAL_OK)
	      {
	          aht_ready_ms = (uint16_t)(HAL_GetTick() - t_power_on);
	          break;
	      }
	      HAL_Delay(25);
	  }

	  /* AHT20 теряет калибровку при каждом отключении питания — обязательно
	   * заново инициализировать после каждого включения PA0, иначе показания мусорные. */
	  HAL_StatusTypeDef aht_init_st = AHT20_Init(&hi2c1);

	  uint8_t aht_status = 0;   /* TEST-ONLY: бит 0x08 = откалиброван, 0x80 = занят */
	  AHT20_ReadStatusByte(&hi2c1, &aht_status);

	  uint8_t usb_state = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9);  // 0 или 1

	  /* Кнопка KEY: нажата = 0 (замкнута на GND). Пока только читаем и логируем,
	   * чтобы убедиться в распиновке до того, как вешать на неё EXTI. */
	  uint8_t btn = HAL_GPIO_ReadPin(WAKE_BTN_GPIO_Port, WAKE_BTN_Pin);

	  AHT20_Data aht;
	  uint32_t aht_i2c_err = 0;
	  /* TEST-ONLY: диагностика чередующихся сбоев AHT20 — код ошибки I2C пишем в CSV,
	   * чтобы понять, что именно происходит на "провальных" циклах. Убрать после отладки. */
	  HAL_StatusTypeDef aht_read_st = AHT20_ReadData(&hi2c1, &aht);
	  if (aht_read_st != HAL_OK)
	  {
	      aht.temperature = 0.0f;
	      aht.humidity    = 0.0f;
	      aht_i2c_err = HAL_I2C_GetError(&hi2c1);
	  }

	  float soil_temp;
	  	  if (!DS18B20_ReadTemp(GPIOA, GPIO_PIN_8, &soil_temp))
	  	      soil_temp = NAN; // датчик не ответил или CRC не сошёлся
	  uint8_t ds_err = (uint8_t)DS18B20_LastError();   /* TEST-ONLY: причина отказа 1-Wire */

	  float soil_hum  = Soil_ReadMoisture();

	  char ts[32];
	  Time_GetTimestamp(ts, sizeof(ts));

	  uint32_t t_sensors_ms = HAL_GetTick() - t_wake;   /* TEST-ONLY */

	  FIL file;
	  res = f_open(&file, "data.csv", FA_OPEN_APPEND | FA_WRITE);
	  if (res == FR_OK)
	  {
	      char line[224];
	      /* TEST-ONLY: хвостовые поля — диагностика сбоев AHT20 и профиль времени цикла:
	       *   aht_ready_ms  — через сколько мс после подачи питания датчик ответил (9999 = не ответил)
	       *   t_sensors_ms  — время от пробуждения до записи (I2C + DS18B20 + ADC)
	       *   t_write_ms    — сколько заняла запись в флеш на ПРОШЛОМ цикле
	       * Убрать вместе с соответствующей логикой после отладки. */
	      snprintf(line, sizeof(line),
	               "%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%lu,%u,%lu,%lu,0x%02X,%u,%u,%u,%u,%lu\r\n",
	               ts,
	               aht.temperature,
	               aht.humidity,
	               soil_temp,
	               soil_hum,
	               usb_state,   // ← добавили состояние PA9
	               (int)aht_init_st,
	               (int)aht_read_st,
	               (unsigned long)aht_i2c_err,
	               (unsigned)aht_ready_ms,
	               (unsigned long)t_sensors_ms,
	               (unsigned long)last_write_ms,
	               (unsigned)aht_status,
	               (unsigned)btn,
	               (unsigned)lse_ok,
	               (unsigned)ds_err,
	               (unsigned)wake_src,
	               (unsigned long)last_hold_ms);

	      uint32_t t_w0 = HAL_GetTick();
	      f_puts(line, &file);
	      f_close(&file);
	      last_write_ms = HAL_GetTick() - t_w0;
	  }

	  /* --- выключаем датчики перед сном (HIGH) --- */
	  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_SET);

	  /* TEST-ONLY: держим МК бодрствующим ещё TEST_AWAKE_HOLD_MS перед следующим STOP —
	   * см. TEST_AWAKE_HOLD_MS выше. */
	  HAL_Delay(TEST_AWAKE_HOLD_MS);
	}
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
  * @brief  Колбэк EXTI: нажата кнопка KEY (PA0).
  *         Само прерывание и будит МК из STOP — здесь только помечаем источник,
  *         чтобы основной цикл понимал, проснулись мы по таймеру или по кнопке.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == WAKE_BTN_Pin)
    {
        woke_by_button = 1;
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
