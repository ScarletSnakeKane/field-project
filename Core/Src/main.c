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
#include "storage_policy.h"
#include "time_service.h"
#include "soil_sensor.h"
#include "battery.h"
#include "ds18b20.h"
#include "power_policy.h"
#include "usbd_core.h"
#include "usbd_def.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
extern USBD_HandleTypeDef hUsbDeviceFS;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Время на стабилизацию питания датчиков (AHT20/DS18B20/почва) после включения PA0. */
#define SENSOR_POWERUP_DELAY_MS   500U

/* Пока кнопка удерживается, МК не уходит в STOP — так USB-сессия не рвётся на время сна.
 * BTN_HOLD_MAX_MS — страховка на случай залипшей/залитой кнопки в поле: по истечении
 * этого времени засыпаем принудительно и больше не ждём кнопку, пока её не отпустят
 * (счёт идёт через все циклы, см. Policy_ButtonKeepsAwake). */
#define BTN_HOLD_POLL_MS   100U
#define BTN_HOLD_MAX_MS    300000U   /* 5 минут */

/* Сколько ждать, пока хост перечислит плату, прежде чем счесть VBUS зарядкой
 * (A9). Windows и Linux укладываются в 1–3 с от подачи подтяжки D+, Android —
 * до ~5 с; 10 с — с запасом. Цена на ложном VBUS — 10 с бодрствования в час,
 * ~30 мкА среднего, против ~10 мА непрерывно раньше. */
#define USB_ENUM_MAX_MS    10000U

/* Шаг пинка сторожа во сне (A4): меньше худшего периода IWDG (~22 с при LSI
 * 47 кГц) с запасом. 3600 и 60 делятся на 15 нацело. Цена — пробуждение на
 * ~0.1–0.3 мс раз в 15 с, доли микроампера среднего (расчёт — в отчёте П4). */
#define WDG_KICK_SEC       15U

/* Потолок размера архива. Тому заявлено 4 МБ ради FAT16 (см. spiflash.h), но
 * физически есть только 2 МБ, из которых на служебные структуры уходит ~50 КБ.
 * Предел взят с запасом ниже физической границы: перешагнув её, запись уходила
 * бы в несуществующие сектора. Достигнув предела, устройство перестаёт писать
 * и продолжает считать неудачи — данные не теряются молча. */
#define CSV_MAX_BYTES   1800000UL

/* Смещение байта типа раздела в MBR: таблица разделов с 446, поле System +4. */
#define MBR_PART_TYPE_OFS   450U

/* Содержимое остаётся CSV, но расширение .txt: на Android с .csv обычно не
 * связано ни одно приложение, и файл видно, а открыть нечем. Текстовую
 * смотрелку телефон имеет всегда, а на компьютере .txt открывается и
 * блокнотом, и Excel через мастер импорта. */
#define DATA_FILE       "data.txt"
#define DATA_FILE_OLD   "data.csv"   /* удаляем при первом запуске после переименования */

/* Набор колонок CSV. Держим одной строкой, чтобы при старте сверить с тем, что
 * уже лежит в файле: состав колонок меняется от версии к версии, а архив теперь
 * переживает перезагрузку — иначе строки разного формата смешались бы в одном файле. */
/* Сначала то, ради чего прибор существует, затем состояние питания, затем
 * диагностика. Отладочные колонки времени цикла и внутренних кодов AHT20 сняты:
 * они своё отработали — за весь снятый архив ни одной ошибки, — а платил за них
 * архив своей длиной. Оставшиеся нужны и в поле: по ним видно, ПОЧЕМУ значение
 * отсутствует, вместо того чтобы гадать над пустой ячейкой.
 *   soil_valid    — 0, если банка просела ниже дропаута LDO и опора АЦП уплыла;
 *   lse_ok        — 0, если часовой кварц не завёлся: тогда времени верить нельзя;
 *   aht_*, ds_err — почему не прочитались воздух и почва;
 *   write_fails   — сколько строк за всю жизнь не удалось записать. */
#define CSV_HEADER "timestamp,air_temp,air_hum,soil_temp,soil_hum," \
                   "bat_pct,vbat,soil_valid,lse_ok," \
                   "aht_init_st,aht_read_st,ds_err,write_fails"

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
static uint8_t fs_ok = 0;    /* смонтирована ли файловая система */
static uint32_t write_fails = 0;  /* накопленное число неудачных записей строки */
static Policy_Button btn_policy;  /* удержание KEY через все циклы сна (A1) */
static uint16_t reset_cause = 0;  /* почему стартовали — Policy_ResetCause */
static uint32_t boot_count  = 0;  /* стартов с последней подачи питания */
static Policy_Usb    usb_policy;  /* VBUS без хоста через все циклы сна (A9) */
static volatile uint8_t woke_by_vbus = 0;    /* фронт VBUS: вставили кабель */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Один цикл сна: погасить периферию, уйти в STOP и подняться обратно.
 * Вынесено из main() отдельной функцией, чтобы первый замер после включения
 * можно было сделать не засыпая, не дублируя при этом весь блок. */
/* Сторожевой таймер (A4). Раньше IWDG запускался только из Error_Handler, а
 * HardFault и любое зависание в активной фазе крутились вечно на полной
 * частоте: прибор стоял на ~10 мА до приезда владельца. Теперь сторож
 * запущен с первой строки main() и кормится в основном цикле, в цикле
 * удержания и в коротких пробуждениях из сна (см. Policy_SleepChunkSec).
 * Период — наибольший: делитель 256, перезагрузка 0xFFF, ~32 с при LSI
 * 32 кГц и ~22 с при 47 кГц (верх допуска); пинок раз в WDG_KICK_SEC.
 * Регистры напрямую: модуль IWDG в HAL этой сборки не включён. Запущенный
 * сторож остановить нельзя — поэтому он замораживается на время остановки
 * ядра отладчиком, иначе каждая точка останова кончалась бы сбросом. */
static void Watchdog_Start(void)
{
  __HAL_DBGMCU_FREEZE_IWDG();

  IWDG->KR  = 0x0000CCCCU;   /* пуск */
  IWDG->KR  = 0x00005555U;   /* разрешить запись PR/RLR */
  IWDG->PR  = 0x06U;         /* делитель 256 */
  IWDG->RLR = 0x0FFFU;       /* максимальная перезагрузка */
  for (volatile uint32_t i = 0U; i < 100000U && (IWDG->SR != 0U); i++)
  {
  }
  IWDG->KR  = 0x0000AAAAU;
}

static void Watchdog_Kick(void)
{
  IWDG->KR = 0x0000AAAAU;
}

/* USB поднимается только из цикла удержания, когда VBUS уже есть, и только
 * после записи цикла (A3). Раньше стек стартовал при каждом пробуждении ДО
 * замера и при старте ДО монтирования: хост, вставленный в этот момент,
 * получал «не готов» посреди записи, а длинное чтение — HARDWARE ERROR.
 * Теперь хост видит том, когда запись цикла уже закончена. */
static uint8_t usb_started = 0;

/* 48 МГц для OTG FS. USB FS требует точности ±0.25 %, а HSI даёт ±1 % при
 * 25 °C и хуже по температуре: в теплице от 5 до 40 °C перечисление и
 * копирование архива срывались бы (A10). На BlackPill есть кварц 25 МГц —
 * PLL на время USB-сессии берём от него: 25 / 25 × 192 / 4 = 48 МГц.
 * Ядро и шины остаются на HSI: PLL нужен только USB, и замерное окно его
 * не ждёт. Выход P (не используется) — тоже 48 МГц, в пределах 84 МГц.
 * HSE не завёлся — откат на HSI, как было раньше: хуже, но не мёртвый USB. */
static void Clock_UsbOn(void)
{
  RCC_OscInitTypeDef osc = {0};

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 25;
  osc.PLL.PLLN = 192;
  osc.PLL.PLLP = RCC_PLLP_DIV4;
  osc.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&osc) == HAL_OK)
      return;

  osc.HSEState = RCC_HSE_OFF;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  osc.PLL.PLLM = 16;
  (void)HAL_RCC_OscConfig(&osc);
}

/* STOP и так останавливает PLL и HSE, но цикл удержания может закончиться
 * и без сна — гасим явно, чтобы не держать их зря. */
static void Clock_UsbOff(void)
{
  RCC_OscInitTypeDef osc = {0};

  /* Сначала PLL, потом его источник: HAL в одном вызове гасит HSE раньше
   * PLL, и тот на миг остался бы без опоры. */
  osc.OscillatorType = RCC_OSCILLATORTYPE_NONE;
  osc.PLL.PLLState = RCC_PLL_OFF;
  (void)HAL_RCC_OscConfig(&osc);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_OFF;
  osc.PLL.PLLState = RCC_PLL_NONE;
  (void)HAL_RCC_OscConfig(&osc);
}

static void Usb_Start(void)
{
  Clock_UsbOn();
  MX_USB_DEVICE_Init();
  usb_started = 1;
}

static void Usb_Stop(void)
{
  USBD_Stop(&hUsbDeviceFS);

  /* Погасить встроенный трансивер вручную: HAL_PCD_DeInit этого не делает,
   * бит PWRDWN остаётся взведённым, и аналоговая часть USB продолжает питаться
   * весь сон. Сделать это надо ДО деинициализации — она отключает тактирование
   * периферии, после чего регистр станет недоступен для записи. */
  USB_OTG_FS->GCCFG &= ~USB_OTG_GCCFG_PWRDWN;

  USBD_DeInit(&hUsbDeviceFS);
  Clock_UsbOff();
  usb_started = 0;
}

/* Возвращает 1, если плата уснула и проснулась, и 0, если сна не было:
 * плата держится на шине, а подошло время очередного замера (C8). Во втором
 * случае USB остаётся поднятым, флеш и шины — как были. */
static uint8_t SleepUntilNextSample(void)
{
  /* --- Не уходим в сон, пока удерживают кнопку или хост держит USB ---
   * Уход в STOP рвёт USB-сессию и портит файл на стороне хоста, поэтому оба
   * признака означают одно: устройство обязано остаться на шине. Кнопка —
   * ручной способ, USB — то же самое автоматически, без нажатий.
   * Датчики к этому моменту уже обесточены, лишнего тока не тратим.
   * Правила — в power_policy.c: потолок кнопки считается через все циклы
   * (btn_policy живёт вне этой функции), иначе залипшая KEY держала бы
   * плату по 5 минут каждый час; VBUS без хоста держит не дольше
   * USB_ENUM_MAX_MS — иначе USB-зарядка или влага на делителе не давали бы
   * спать вовсе. */
  uint32_t t_prev  = HAL_GetTick();
  uint32_t t_awake = t_prev;   /* замер только что был — отсчёт интервала отсюда */
  uint8_t  vbus_edge = woke_by_vbus;
  woke_by_vbus = 0;
  while (1)
  {
      uint8_t btn_held    = (HAL_GPIO_ReadPin(WAKE_BTN_GPIO_Port, WAKE_BTN_Pin) == GPIO_PIN_RESET);
      uint8_t usb_present = (HAL_GPIO_ReadPin(VBUS_GPIO_Port, VBUS_Pin) == GPIO_PIN_SET);
      uint8_t usb_config  = usb_started && (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);

      uint32_t now = HAL_GetTick();
      uint8_t btn_awake = Policy_ButtonKeepsAwake(&btn_policy, btn_held, now - t_prev, BTN_HOLD_MAX_MS);
      uint8_t usb_awake = Policy_UsbKeepsAwake(&usb_policy, usb_present, usb_config,
                                               vbus_edge, now - t_prev, USB_ENUM_MAX_MS);
      t_prev = now;
      vbus_edge = woke_by_vbus;   /* фронт за время ожидания — новый шанс хосту */
      woke_by_vbus = 0;

      Watchdog_Kick();   /* сессия с хостом может длиться сутки */

      if (!btn_awake && !usb_awake)
          break;

      /* Поднимаем стек, только пока у хоста есть шанс: VBUS, признанный
       * зарядкой, USB не поднимает (Policy_UsbKeepsAwake вернул 0). */
      if (usb_awake && !usb_started)
          Usb_Start();

      if (Policy_MeasureDue(now - t_awake, RTC_WAKEUP_INTERVAL_SEC * 1000UL))
          return 0;

      /* Ждём во сне ядра (WFI), а не в занятом цикле HAL_Delay: SysTick будит
       * каждую миллисекунду, а между тиками ядро не жжёт ток впустую. */
      while (HAL_GetTick() - now < BTN_HOLD_POLL_MS)
          __WFI();
  }

  /* Флеш — на постоянном питании, гасить её нечем, кроме Deep Power-Down.
   * Делаем это только сейчас: пока шла USB-сессия (условие выше),
   * хост мог читать диск, и усыплять флеш было нельзя. */
  SPI_Flash_DeepPowerDown();

  /* Гасим периферию шин. Порядок важен: SPI деинициализируем строго ПОСЛЕ
   * команды Deep Power-Down выше — иначе её нечем было бы отправить. */
  HAL_SPI_DeInit(&hspi1);
  HAL_I2C_DeInit(&hi2c1);

  /* Гасим USB-стек, если он поднимался. Досюда мы доходим только когда
   * хост не держит сессию, и рвать нечего. Аналоговая часть трансивера OTG FS
   * питается от VDD и продолжает потреблять в STOP, даже когда тактирование
   * снято. */
  if (usb_started)
      Usb_Stop();

  /* Парковка выводов — строго ПОСЛЕ всех деинициализаций. Каждая из них внутри
   * себя дёргает HAL_GPIO_DeInit и возвращает свои выводы в плавающий вход,
   * так что любая парковка, сделанная раньше, была бы просто затёрта. */
  MX_GPIO_SleepPrepare();

  /* Отключаем трассировочный блок ядра. С поднятым TRCENA отладочная логика
   * остаётся под питанием и во сне — это сотни микроампер на ровном месте.
   * DWT нужен только для микросекундных задержек 1-Wire в активной фазе. */
  DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
  CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;

  woke_by_button = 0;   /* сбрасываем перед сном: интересен источник ЭТОГО пробуждения */
  woke_by_vbus   = 0;
  (void)RTC_TakeWakeup();

  /* Залипшую кнопку на время сна отключаем от подтяжки и EXTI (A1): через
   * замкнутый контакт иначе тёк бы ток весь час, а её «нажатие» больше
   * ничего не значит. Будит только RTC; отпущена ли она — проверим после. */
  uint8_t btn_parked = btn_policy.stuck;
  if (btn_parked)
      MX_GPIO_ButtonPark();

  /* Отладочный блок не должен переживать STOP. Программатор при подключении
   * выставляет DBG_STOP, чтобы можно было ставить точки останова в спящем МК,
   * и этот бит держит отладочную логику и тактирование живыми весь сон —
   * сотни микроампер. Сам по себе он сбрасывается только по сбросу питания,
   * поэтому после отладки прибор может месяцами жить с включённой отладкой
   * и никто не заметит. Гасим явно на каждом цикле. */
  HAL_DBGMCU_DisableDBGStopMode();

  /* Обесточить внутреннюю флеш-память на время STOP. Иначе её стабилизатор
   * остаётся под нагрузкой всю ночь ради содержимого, которое всё равно не
   * читается: ядро стоит. Платим за это лишними микросекундами на пробуждение —
   * флеш надо снова поднять, — что на фоне часового интервала незаметно. */
  HAL_PWREx_EnableFlashPowerDown();

  /* --- уход в сон: STOP mode, будит RTC Wakeup Timer, кнопка или VBUS ---
   * Интервал RTC_WAKEUP_INTERVAL_SEC режется на куски по WDG_KICK_SEC (A4):
   * сторож в STOP не замирает и часа не переживёт. Между кусками — короткое
   * пробуждение на HSI: пинок сторожу и сразу обратно, без замера и без
   * периферии. Таймер перевзводится заново перед первым куском, чтобы
   * длительность сна не «плавала» от фазы свободно бегущего таймера. */
  uint32_t left  = RTC_WAKEUP_INTERVAL_SEC;
  uint32_t armed = 0;
  while (left > 0U)
  {
      uint32_t chunk = Policy_SleepChunkSec(left, WDG_KICK_SEC);
      if (chunk != armed)
      {
          /* Перевзвод ждёт флага RTC с таймаутом по тику — тик на это время
           * возвращаем, иначе таймаут не сработал бы никогда (см. ниже).
           * Счётчик CK_SPRE даёт (WUT + 1) с, отсюда «- 1». */
          HAL_ResumeTick();
          HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
          HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, chunk - 1U, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
          armed = chunk;
      }

      Watchdog_Kick();
      HAL_SuspendTick();
      HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

      /* Кнопка и VBUS — это человек: просыпаемся на замер сразу. */
      if (woke_by_button || woke_by_vbus)
          break;

      /* Не таймер и не человек — случайное прерывание; кусок не отработан,
       * спим дальше, таймер продолжает считать с того же места. */
      if (!RTC_TakeWakeup())
          continue;
      left -= chunk;

      /* Залипшую кнопку могли отпустить. Проверка — доли микросекунды на
       * подтяжке; отпустили — оставляем EXTI включённым до конца сна. */
      if (btn_parked && left > 0U)
      {
          MX_GPIO_ButtonRestore();
          MX_GPIO_ButtonSettle();
          if (HAL_GPIO_ReadPin(WAKE_BTN_GPIO_Port, WAKE_BTN_Pin) == GPIO_PIN_SET)
          {
              (void)Policy_ButtonKeepsAwake(&btn_policy, 0, 0, BTN_HOLD_MAX_MS);
              btn_parked = 0;
          }
          else
          {
              MX_GPIO_ButtonPark();
          }
      }
  }
  Watchdog_Kick();

  /* --- пробуждение: ядро на HSI без PLL. Шины и задержки работают от того
   * же HSI 16 МГц, что и до сна (делители и латентность флеш STOP сохраняет),
   * так что SystemClock_Config здесь больше не нужна: PLL нужен только USB,
   * и его поднимает Usb_Start. Раньше она же на каждом пробуждении заново
   * запускала LSE, и один отказ кварца уводил в Error_Handler (A5). --- */
  /* Тик поднимаем ДО настройки тактирования, а не после. Все ожидания внутри
   * HAL_RCC_OscConfig/ClockConfig отмеряют свой таймаут по HAL_GetTick(), а он
   * растёт только в прерывании SysTick — том самом, которое HAL_SuspendTick()
   * выключил перед сном. Пока тик стоит, разность (HAL_GetTick() - tickstart)
   * всегда ноль, условие выхода по таймауту не выполняется НИКОГДА, и отказ
   * запуска генератора превращался бы не в HAL_TIMEOUT, а в вечный горячий
   * цикл прямо внутри драйвера. То же относится к Usb_Start, который
   * поднимает HSE и PLL. Частота та же, что была до сна (HSI 16 МГц), поэтому
   * перенастраивать SysTick не нужно: достаточно вернуть его прерывание. */
  HAL_ResumeTick();

  /* Таймер куска на время бодрствования не нужен: его прерывание с высшим
   * приоритетом каждые WDG_KICK_SEC вклинивалось бы в тайминги 1-Wire.
   * Сторожа в активной фазе кормит основной цикл. */
  HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);

  /* В аналоге PA0 читается нулём, то есть «зажата», — вернуть подтяжку до
   * того, как цикл удержания спросит кнопку, иначе залипание не кончилось
   * бы никогда. */
  if (btn_parked)
  {
      MX_GPIO_ButtonRestore();
      MX_GPIO_ButtonSettle();
  }

  /* Возвращаем выводы 1-Wire из аналога и поднимаем SPI — обязательно до
   * любого обращения к флеш, иначе команду пробуждения будет некому послать. */
  MX_GPIO_SleepRestore();
  MX_SPI1_Init();

  /* Будим флеш до любого обращения к ней (в DPD она игнорирует все команды, кроме 0xAB). */
  SPI_Flash_ReleasePowerDown();

  /* Возвращаем трассировочный блок: без него не работают задержки 1-Wire. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* USB здесь не поднимаем: его стартует цикл удержания после записи, если
   * есть VBUS (A3). Кабель, вставленный во время замера, просто подождёт
   * до конца записи — EXTI по VBUS к тому времени уже разбудил плату. */
  return 1;
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
    Watchdog_Start();   /* до всего остального: зависнуть может уже старт */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // разрешить трассировочный блок ядра
    DWT->CYCCNT = 0;                                 // сбросить счётчик тактов
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;             // запустить счётчик тактов
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Причина сброса и код сбоя — до MX_RTC_Init: смена источника тактирования
   * RTC (A5) сбрасывает backup-домен, и код из DR1 пропал бы. */
  reset_cause = Policy_ResetCause(RCC->CSR, RTC->BKP1R);
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_FATFS_Init();
  /* MX_USB_DEVICE_Init() отсюда убран (A3): USB стартует цикл удержания после
   * первой записи. В FieldSensor.ioc для него стоит выставить «Do not
   * generate function call», иначе CubeMX вернёт вызов при перегенерации. */
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_RTC_Init();
  // LSE НЕ запустился → RTC идёт от LSI → метки времени уплывают. Плата при
  // этом живёт и пишет (A5), а флаг уходит в каждую строку CSV.
  lse_ok = RTC_LseOk();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */

  /* Кнопочного жеста «стереть архив» больше нет (A2/C4). Он срабатывал от
   * зажатой KEY при любом сбросе, кроме программного: при замене банки,
   * просадке, перезапуске сторожем — а залитая водой кнопка «зажата» всегда.
   * Архив теперь стирает только владелец: удаляет data.txt с компьютера,
   * подключив прибор по USB. KEY при старте ничего не значит. */
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* Счётчик стартов с последней подачи питания (DR2) и сброс кода сбоя (DR1),
   * чтобы следующий старт не унаследовал чужую причину. Backup-домен сидит
   * на VDD (VBAT не отдельный), так что подача питания обнуляет и счётчик —
   * это и нужно: он считает именно тихие перезагрузки. */
  HAL_PWR_EnableBkUpAccess();
  boot_count = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2) + 1U;
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, boot_count);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, 0U);
  Watchdog_Kick();   /* запуск LSE в MX_RTC_Init мог занять до 10 с */

  // Включаем питание датчиков (LOW)
  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);

  // === 0) Часы ===
  // Календарь живёт в backup-домене и переживает сброс, поэтому проверки
  // "не инициализирован" мало: после обычного ресета время идёт дальше, а вот
  // новая прошивка должна переставить его на своё время сборки.
  Time_SyncToBuildIfNeeded();

  /* Флеш занята основным потоком с этого места и до конца первой дозаписи
   * в цикле (первый проход идёт без сна, так что это пара секунд). USB до
   * этого не поднимается вовсе (A3); флаг — вторая линия обороны на случай,
   * если порядок когда-нибудь снова поменяют: хост не должен читать том,
   * пока его монтируют, сверяют заголовок и пишут первую строку. Флаг
   * снимает блок дозаписи. */
  SPI_Flash_SetBusy(1);

  // 1) Инициализация SPI флеш
  SPI_Flash_Init();

  // 2) Монтируем диск. Форматируем ТОЛЬКО если файловой системы на флеш нет:
  //    безусловный f_mkfs стирал накопленные данные при каждом старте, то есть
  //    любой сброс или севшая и заменённая батарея в поле обнуляли весь архив.
  //    До трёх попыток с повторной инициализацией флеш: одно сорванное чтение
  //    после просадки питания не должно решать судьбу тома.
  for (uint8_t attempt = 0; attempt < 3; attempt++)
  {
      if (attempt > 0)
      {
          f_mount(NULL, USERPath, 0);
          SPI_Flash_Init();
      }
      res = f_mount(&USERFatFS, USERPath, 1);
      if (res == FR_OK)
          break;
  }
  Watchdog_Kick();

  /* Раньше здесь всё, что не FAT16, принудительно переформатировалось — ради
   * Android, который FAT12 не монтирует. Это стирало рабочий архив любой
   * старой прошивки. Теперь существующий FAT12 монтируется и пишется как
   * есть; Android его не увидит, Windows и Linux читают. Перевести такой
   * прибор на FAT16 можно только сознательно, сняв архив и стерев флеш. */

  /* Питание для форматирования и правки MBR — по тому же правилу, что и для
   * дозаписи: обе операции стирают 4-КБ блоки со служебными структурами. */
  Battery_Data bat_boot;
  Battery_Read(&bat_boot);
  uint8_t boot_power_ok = Storage_MayWrite(bat_boot.valid, bat_boot.low, bat_boot.vdd);

  if (res == FR_NO_FILESYSTEM)
  {
      /* «Нет ФС» по мнению FatFs — ещё не повод стирать: так же выглядит
       * одно искажённое чтение или прерванная перезапись блока 0. Форматируем
       * только чистую флеш — правило и его обоснование в storage_policy.h. */
      BYTE *mbr  = work;
      BYTE *boot = work + FLASH_SECTOR_BYTES;
      BYTE *root = work + 2 * FLASH_SECTOR_BYTES;
      uint8_t reads_ok =
          (disk_read(0, mbr, 0, 1) == RES_OK) &&
          (disk_read(0, boot, STORAGE_BOOT_SECTOR, 1) == RES_OK);

      uint8_t may_format = Storage_MayFormat(1, reads_ok, mbr, boot);

      /* Не чистая — но, может быть, это наш же формат, сорванный между
       * загрузочным сектором и MBR (R3). Без этой проверки такой прибор
       * больше никогда не форматировался бы и молча не писал ничего. */
      if (!may_format && reads_ok)
      {
          uint32_t root_lba = Storage_OurRootDirSector(boot);
          uint8_t  root_ok  = (root_lba != 0) &&
                              (disk_read(0, root, root_lba, 1) == RES_OK);
          may_format = Storage_MayRedoFormat(1, reads_ok, root_ok, mbr, boot, root);
      }

      if (boot_power_ok && may_format)
      {
          /* Решение «форматировать» принято по содержимому флеш один раз;
           * повторы его не пересматривают — после первой попытки загрузочный
           * сектор уже записан. Повтор нужен потому, что f_mkfs теперь может
           * вернуть FR_DISK_ERR по несошедшейся сверке блока, а без него
           * одна такая сверка оставляла прибор без тома до следующего старта.
           * Перед повтором питание проверяется заново: если сбой был от
           * просадки, долбить флеш дальше нельзя. */
          for (uint8_t attempt = 0; attempt < 3; attempt++)
          {
              if (attempt > 0)
              {
                  Battery_Read(&bat_boot);
                  boot_power_ok = Storage_MayWrite(bat_boot.valid, bat_boot.low, bat_boot.vdd);
                  if (!boot_power_ok)
                      break;
                  SPI_Flash_Init();
              }
              /* Размер кластера задан явно — 512 байт, минимум. Автовыбор для тома 4 МБ
               * взял бы 1 КБ, кластеров получилось бы около 4050 при пороге FAT16 в 4085,
               * и том снова вышел бы FAT12 — тот самый, который Android не монтирует. */
              Watchdog_Kick();
              res = f_mkfs(USERPath, FM_FAT, FLASH_SECTOR_BYTES, work, sizeof(work));
              if (res == FR_OK)
              {
                  res = f_mount(&USERFatFS, USERPath, 1);
                  break;
              }
          }
      }
  }

  /* Раньше здесь стоял while(1) — отказ монтирования означал вечное зависание
   * в активном режиме, то есть посаженную за пару дней батарею и полную тишину
   * в данных. Теперь это не фатально: помечаем ФС как неисправную и пробуем
   * перемонтировать раз в цикл, засыпая между попытками. Нечистая флеш без
   * распознаваемой ФС так и остаётся с fs_ok = 0: архив на ней не трогаем.
   * Сорванный в этой загрузке формат доделается на следующем старте —
   * его узнаёт Storage_MayRedoFormat. */
  fs_ok = (res == FR_OK) ? 1U : 0U;

  /* Android считает раздел пригодным для монтирования, только если байт типа
   * в таблице MBR входит в его короткий список: 0x0B, 0x0C, 0x0E (FAT) или
   * 0x83 (Linux). FatFs для тома меньше 65536 секторов проставляет 0x04 —
   * совершенно законный FAT16, который Windows читает без единого вопроса,
   * а vold молча пропускает и показывает «Unsupported USB drive».
   * Переписываем тип на 0x0E (FAT16 LBA). Правка затрагивает один байт в MBR,
   * данные и файловую систему не трогает и повторное выполнение безвредно.
   * Только для FAT16: у оставленного как есть FAT12 тип 0x01, и 0x0E был бы
   * для него ложью. */
  if (fs_ok && boot_power_ok && USERFatFS.fs_type == FS_FAT16 &&
      disk_read(0, work, 0, 1) == RES_OK)
  {
      if (work[MBR_PART_TYPE_OFS] != 0x0E)
      {
          work[MBR_PART_TYPE_OFS] = 0x0E;
          disk_write(0, work, 0, 1);
      }
  }

  // 2b) Архив начинается заново в двух случаях:
  //     — владелец удалил data.txt с компьютера по USB — это единственный
  //       способ стереть архив; прошивка сама его не удаляет никогда;
  //     — заголовок в файле не совпал с текущим (залита версия с другим составом
  //       колонок) — иначе строки разного формата смешались бы в одном файле.
  uint8_t header_mismatch = 0;
  if (fs_ok)
  {
      FIL hf;
      if (f_open(&hf, DATA_FILE, FA_READ) == FR_OK)
      {
          char hdr[sizeof(CSV_HEADER) + 8];
          header_mismatch = (f_gets(hdr, sizeof(hdr), &hf) == NULL) ||
                            (strncmp(hdr, CSV_HEADER, strlen(CSV_HEADER)) != 0);
          f_close(&hf);
      }
  }

  if (fs_ok && header_mismatch)
      f_unlink(DATA_FILE);

  /* Файл переименован из .csv в .txt — старый остался бы на диске мёртвым грузом
   * и сбивал бы с толку при просмотре с телефона. Удаляем один раз; если его нет,
   * вызов просто вернёт ошибку, которая нам безразлична. */
  if (fs_ok)
      f_unlink(DATA_FILE_OLD);

  // 3) Создаём CSV файл с заголовком, если файла нет
  res = f_open(&file, DATA_FILE, FA_OPEN_EXISTING | FA_WRITE);

  if (res == FR_NO_FILE)
  {
      // Файл отсутствует → создаём новый и пишем заголовок
      res = f_open(&file, DATA_FILE, FA_OPEN_APPEND | FA_WRITE);
      if (res == FR_OK)
      {
          f_puts(CSV_HEADER "\n", &file);
          f_close(&file);
      }
  }
  else if (res == FR_OK)
  {
      // Файл существует → просто закрываем
      f_close(&file);
  }

  /* Датчики поднимает и гасит сам цикл, здесь они не нужны. Явно обесточиваем
   * их перед первым сном: MX_GPIO_Init() оставляет питание включённым, и без
   * этой строки первый интервал сна (в поле — целый час) проходил бы с
   * запитанными AHT20, DS18B20 и датчиком почвы. */
  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_SET);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /* Первый проход замеряет сразу, без сна. Иначе после включения архив
   * оставался бы пустым весь первый интервал — в поле это час, в течение
   * которого исправный прибор неотличим от мёртвого. Одна строка при
   * установке стоит копейки и сразу подтверждает, что прибор жив. */
  uint8_t first_pass = 1;

  while (1)
  {
	  if (!first_pass)
	      SleepUntilNextSample();
	  first_pass = 0;
	  Watchdog_Kick();

	  /* --- включаем датчики (LOW) --- */
	  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);
	  /* Полная пауза на выход датчиков в режим, а не символические 50 мс.
	   * Константа SENSOR_POWERUP_DELAY_MS была заведена ровно под это, но в коде
	   * не использовалась, и AHT20 опрашивался слишком рано: адрес он
	   * подтверждает почти сразу, а на самокалибровку ему нужно заметно больше,
	   * и AHT20_Init не укладывался в свой бюджет попыток. За двое суток
	   * боевого прогона так потерялись две трети замеров воздуха.
	   * Цена паузы: 450 мс раз в час при токе активной фазы порядка 20 мА —
	   * это около 2.5 мкА среднего против бюджета в 377 мкА. */
	  HAL_Delay(SENSOR_POWERUP_DELAY_MS);

	  /* Датчик на шине I2C1 только что был полностью обесточен. Жёстко сбрасываем саму
	   * периферию I2C1 через RCC перед повторным использованием — иначе она может остаться
	   * в залипшем BUSY-состоянии после неудачной транзакции прошлого цикла и больше
	   * никогда не восстановиться сама (обычный DeInit/Init этого не гарантирует). */
	  HAL_I2C_DeInit(&hi2c1);
	  __HAL_RCC_I2C1_FORCE_RESET();
	  __HAL_RCC_I2C1_RELEASE_RESET();
	  MX_I2C1_Init();

	  /* Ждём, пока AHT20 реально начнёт подтверждать свой адрес на шине, вместо
	   * слепой фиксированной паузы: время выхода на режим зависит от температуры
	   * и от того, насколько успел разрядиться его питающий конденсатор. */
	  for (uint16_t i = 0; i < 80; i++)   /* до ~2 секунд ожидания */
	  {
	      if (HAL_I2C_IsDeviceReady(&hi2c1, AHT20_ADDR, 1, 10) == HAL_OK)
	          break;
	      HAL_Delay(25);
	  }

	  /* AHT20 теряет калибровку при каждом отключении питания — обязательно
	   * заново инициализировать после каждого включения PA0, иначе показания мусорные. */
	  HAL_StatusTypeDef aht_init_st = AHT20_Init(&hi2c1);

	  /* Если датчик не откалибровался, читать его бессмысленно: транзакции
	   * пройдут успешно, а в кадре будут нули. Раньше чтение выполнялось
	   * независимо от исхода инициализации, и такой кадр однажды разобрался
	   * в -50.00 °C и ушёл в архив как настоящее измерение. */
	  AHT20_Data aht;
	  HAL_StatusTypeDef aht_read_st = (aht_init_st == HAL_OK)
	                               ? AHT20_ReadData(&hi2c1, &aht)
	                               : HAL_ERROR;
	  if (aht_read_st != HAL_OK)
	  {
	      /* Не нули, а NAN. Ноль градусов в теплице — совершенно правдоподобное
	       * значение, и отказ датчика выглядел в архиве как настоящий заморозок;
	       * ровно на этом однажды уже обожглись с DS18B20. NAN спутать не с чем,
	       * и почва рядом помечается им же. */
	      aht.temperature = NAN;
	      aht.humidity    = NAN;
	  }

	  float soil_temp;
	  	  if (!DS18B20_ReadTemp(GPIOA, GPIO_PIN_8, &soil_temp))
	  	      soil_temp = NAN; // датчик не ответил или CRC не сошёлся
	  uint8_t ds_err = (uint8_t)DS18B20_LastError();   /* почему 1-Wire не ответил */

	  float soil_hum  = Soil_ReadMoisture();

	  Battery_Data bat;
	  Battery_Read(&bat);
	  Watchdog_Kick();   /* опрос датчиков — до нескольких секунд; запись впереди */

	  char ts[32];
	  Time_GetTimestamp(ts, sizeof(ts));

	  /* На просевшем питании не пишем вовсе: каждая дозапись стирает и
	   * перепрограммирует 4-КБ блоки каталога и FAT, и сбой посреди них теряет
	   * весь data.txt. Правило — в storage_policy.h. Пропуски считаем отдельно
	   * от write_fails: это не отказ тома. В CSV счётчик пока не выводится
	   * (формат строки — не этого блока), его видно отладчиком. */
	  static volatile uint32_t lowpower_skips = 0;
	  uint8_t power_ok = Storage_MayWrite(bat.valid, bat.low, bat.vdd);

	  /* Флеш наша до конца блока: MSC в это время отвечает хосту «не готов». */
	  SPI_Flash_SetBusy(1);

	  /* Пока шла USB-сессия, хост читал флеш в обход FatFs. Сам том он не
	   * меняет (только чтение), но кэш FatFs — окно сектора и FSINFO — мог
	   * разойтись с флеш после сбоев прошлых циклов; перемонтирование дёшево
	   * и гарантирует, что дозапись начнётся с того, что лежит на флеш. */
	  if (SPI_Flash_TakeHostSession() && fs_ok)
	  {
	      f_mount(NULL, USERPath, 0);
	      fs_ok = 0;
	  }

	  /* Если ФС не смонтирована (сбой при старте или отвалилась позже) — пробуем
	   * поднять её заново, но не чаще одного раза за цикл. Между попытками
	   * устройство спит, поэтому даже неустранимая поломка не сажает батарею. */
	  if (!fs_ok)
	      fs_ok = (f_mount(&USERFatFS, USERPath, 1) == FR_OK) ? 1U : 0U;

	  FIL file;
	  res = !power_ok ? FR_DENIED
	      : fs_ok     ? f_open(&file, DATA_FILE, FA_OPEN_APPEND | FA_WRITE)
	                  : FR_NOT_READY;

	  /* Архив заполнен — закрываем файл и не пишем: следующая строка ушла бы
	   * за физическую границу флеш. Счётчик отказов продолжает расти, поэтому
	   * по последним записанным строкам видно, что запись прекратилась. */
	  if (res == FR_OK && f_size(&file) >= CSV_MAX_BYTES)
	  {
	      f_close(&file);
	      res = FR_DENIED;
	  }

	  if (res == FR_OK)
	  {
	      char line[128];
	      /* Перевод строки задаём одним "\n": FatFs собран с _USE_STRFUNC = 2,
	       * и f_puts сам разворачивает \n в \r\n. Со старым "\r\n" в формате на
	       * диск уходило "\r\r\n" — лишний байт в каждой строке и мусорный
	       * символ в конце последнего поля при разборе строгим парсером. */
	      snprintf(line, sizeof(line),
	               "%s,%.2f,%.2f,%.2f,%.2f,%.0f,%.2f,%u,%u,%d,%d,%u,%lu\n",
	               ts,
	               aht.temperature,
	               aht.humidity,
	               soil_temp,
	               soil_hum,
	               /* Отказ замера батареи — nan в обеих колонках, как у остальных
	                * датчиков. Раньше здесь печаталось "0,0.00" — «батарея пуста»,
	                * и прогноз ресурса строился на ложной точке. Процент выводится
	                * через %.0f ради того же nan; для целых 0..100 вывод прежний. */
	               bat.valid ? (double)bat.percent : (double)NAN,
	               bat.valid ? (double)bat.volts   : (double)NAN,
	               /* soil_valid: ниже дропаута LDO опора АЦП уплывает вместе с банкой,
	                * поэтому влажность почвы с этого момента недостоверна. */
	               (unsigned)(bat.valid && !bat.low),
	               (unsigned)lse_ok,
	               (int)aht_init_st,
	               (int)aht_read_st,
	               (unsigned)ds_err,
	               (unsigned long)write_fails);

	      int      puts_res  = f_puts(line, &file);
	      FRESULT  close_res = f_close(&file);

	      /* Раньше результат записи игнорировался: заполнившийся том или сбой SPI
	       * молча съедали строку, и в данных это выглядело просто как её отсутствие.
	       * Считаем отказы и требуем перемонтирования на следующем цикле — том мог
	       * отвалиться, и переподключение это чинит. */
	      if (puts_res < 0 || close_res != FR_OK)
	      {
	          write_fails++;
	          fs_ok = 0;
	      }
	  }
	  else if (!power_ok)
	  {
	      lowpower_skips++;
	  }
	  else
	  {
	      write_fails++;
	      /* Заполненный архив — не поломка тома, перемонтирование тут не поможет
	       * и только зря потратит энергию на каждом цикле. */
	      if (res != FR_DENIED)
	          fs_ok = 0;   /* не открылось — пробуем перемонтировать в следующем цикле */
	  }

	  /* Здесь же снимается и флаг, выставленный при старте (см. блок монтирования). */
	  SPI_Flash_SetBusy(0);

	  /* --- выключаем датчики перед сном (HIGH) --- */
	  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_SET);
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
  /* LSE отсюда убран (A5): его запускает MX_RTC_Init один раз, с откатом на
   * LSI. Здесь его отказ уводил в Error_Handler — цикл перезагрузок без
   * единой строки в архиве. CubeMX при перегенерации вернёт LSE сюда —
   * после перегенерации эту правку повторить. */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
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
    else if (GPIO_Pin == VBUS_Pin)
    {
        /* Кабель вставили: даже если прошлый VBUS признан зарядкой, у этого
         * хоста свой шанс перечислить плату (Policy_UsbKeepsAwake). */
        woke_by_vbus = 1;
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
  /* Заготовка CubeMX здесь была `__disable_irq(); while(1){}` — вечный цикл на
   * полной частоте. Для прибора, который должен год простоять в теплице без
   * присмотра, это самый дорогой вид отказа: наружу он ничего не сообщает,
   * архив просто перестаёт расти, банка садится за считанные дни на ~8 мА, и
   * заказчик узнаёт об этом, только приехав за данными через месяц.
   *
   * Раньше сюда вёл отказ кварца 32.768 кГц на любом пробуждении; теперь он
   * не фатален (A5, rtc.c), но остаются отказы инициализации I2C, SPI, RTC,
   * ADC и USB.
   *
   * Поэтому вместо цикла — перезапуск сторожевым таймером, причём ожидание
   * сброса проходит в STOP, а не на полной частоте. IWDG тактируется от LSI,
   * то есть не зависит от того, что именно отказало в тракте HSE/LSE/PLL, и
   * продолжает считать в STOP. Максимальный период при делителе 256 и
   * LSI 32 кГц — около 32 с: неустранимый отказ превращается в редкий цикл
   * "сброс — попытка — сон" за микроамперы вместо непрерывных миллиампер,
   * а разовый сбой просто вылечивается перезапуском.
   *
   * Регистры трогаем напрямую, без HAL_IWDG_Init: сюда мы попадаем из
   * поломанного состояния, и полагаться на инициализированные дескрипторы
   * нельзя. Отдельного тактирования у IWDG нет, он доступен всегда. */
  __disable_irq();

  /* Все прерывания — и в NVIC: при запрещённых прерываниях WFI всё равно
   * просыпается от любого ожидающего, а таймер куска сна (A4) или VBUS
   * держали бы ожидающее вечно — и STOP ниже превратился бы в горячий цикл
   * до самого сброса. Запрещённое в NVIC прерывание WFI не будит. */
  for (uint32_t i = 0U; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++)
  {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }

  /* Причина — в backup-регистр: после перезапуска она уйдёт в reset_cause
   * каждой строки, и цикл «сбой — перезапуск» будет виден в архиве. */
  RCC->APB1ENR |= RCC_APB1ENR_PWREN;
  PWR->CR |= PWR_CR_DBP;
  RTC->BKP1R = POLICY_FAULT_MAGIC | POLICY_FAULT_ERROR;

  /* Сторож обычно уже запущен (Watchdog_Start); повторный пуск безвреден и
   * нужен, если сюда попали раньше него. */
  IWDG->KR  = 0x0000CCCCU;   /* пуск */
  IWDG->KR  = 0x00005555U;   /* разрешить запись PR/RLR */
  IWDG->PR  = 0x06U;         /* делитель 256 */
  IWDG->RLR = 0x0FFFU;       /* максимальная перезагрузка */

  /* Ждём готовности регистров ограниченным счётчиком, а не по HAL_GetTick():
   * тик здесь может быть выключен, и ожидание по нему не кончилось бы никогда —
   * ровно та ошибка, из-за которой мы тут и оказались. */
  for (volatile uint32_t i = 0U; i < 100000U && (IWDG->SR != 0U); i++)
  {
  }

  IWDG->KR = 0x0000AAAAU;    /* перезагрузить счётчик */

  /* Сюда приходят из инициализации ADC, I2C, SPI, RTC и USB, то есть уже после
   * SystemClock_Config, где тактирование PWR включается первой же строкой. Но
   * зависеть от этого в аварийном обработчике не хочется: без тактирования PWR
   * вход в STOP молча не сработает, и вместо экономного ожидания сброса мы
   * получили бы тот самый горячий цикл, от которого уходим. Одна строка снимает
   * вопрос. */
  __HAL_RCC_PWR_CLK_ENABLE();

  /* Возвращаться из STOP сюда мы не рассчитываем — раньше придёт сброс. Но если
   * какое-то прерывание всё же поднимет ядро, снова уходим в STOP, а не
   * начинаем греться. */
  while (1)
  {
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
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
