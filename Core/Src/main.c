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
#include "battery.h"
#include "ds18b20.h"
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
#define CSV_HEADER "timestamp,air_temp,air_hum,soil_temp,soil_hum," \
                   "aht_init_st,aht_read_st,aht_i2c_err,aht_ready_ms," \
                   "t_sensors_ms,t_write_ms,aht_status,btn,lse_ok,ds_err," \
                   "wake_src,hold_ms,write_fails," \
                   "bat_pct,vbat,vdd,soil_valid"

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

  // Состояние кнопки на момент старта читаем сразу: это жест «стереть архив»,
  // и пользователь отпустит кнопку через мгновение после сброса.
  uint8_t wipe_requested = (HAL_GPIO_ReadPin(WAKE_BTN_GPIO_Port, WAKE_BTN_Pin) == GPIO_PIN_RESET);

  // Включаем питание датчиков (LOW)
  HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);

  // === 0) Часы ===
  // Календарь живёт в backup-домене и переживает сброс, поэтому проверки
  // "не инициализирован" мало: после обычного ресета время идёт дальше, а вот
  // новая прошивка должна переставить его на своё время сборки.
  Time_SyncToBuildIfNeeded();

  // 1) Инициализация SPI флеш
  SPI_Flash_Init();

  // 2) Монтируем диск. Форматируем ТОЛЬКО если файловой системы на флеш нет:
  //    безусловный f_mkfs стирал накопленные данные при каждом старте, то есть
  //    любой сброс или севшая и заменённая батарея в поле обнуляли весь архив.
  res = f_mount(&USERFatFS, USERPath, 1);

  /* Смонтировавшийся том ещё не значит подходящий: на флеш может лежать старый
   * FAT12, созданный до перехода на заявленные 4 МБ. Он прекрасно читается
   * Windows и так же исправно отвергается Android, поэтому переформатируем
   * всё, что не FAT16. Проверка нужна ровно один раз после этой прошивки,
   * но стоит дёшево и защищает от повторения истории. */
  if (res == FR_OK && USERFatFS.fs_type != FS_FAT16)
      res = FR_NO_FILESYSTEM;

  if (res == FR_NO_FILESYSTEM)
  {
      /* Размер кластера задан явно — 512 байт, минимум. Автовыбор для тома 4 МБ
       * взял бы 1 КБ, кластеров получилось бы около 4050 при пороге FAT16 в 4085,
       * и том снова вышел бы FAT12 — тот самый, который Android не монтирует. */
      res = f_mkfs(USERPath, FM_FAT, FLASH_SECTOR_BYTES, work, sizeof(work));
      if (res == FR_OK)
          res = f_mount(&USERFatFS, USERPath, 1);
  }

  /* Раньше здесь стоял while(1) — отказ монтирования означал вечное зависание
   * в активном режиме, то есть посаженную за пару дней батарею и полную тишину
   * в данных. Теперь это не фатально: помечаем ФС как неисправную и пробуем
   * перемонтировать раз в цикл, засыпая между попытками. */
  fs_ok = (res == FR_OK) ? 1U : 0U;

  /* Android считает раздел пригодным для монтирования, только если байт типа
   * в таблице MBR входит в его короткий список: 0x0B, 0x0C, 0x0E (FAT) или
   * 0x83 (Linux). FatFs для тома меньше 65536 секторов проставляет 0x04 —
   * совершенно законный FAT16, который Windows читает без единого вопроса,
   * а vold молча пропускает и показывает «Unsupported USB drive».
   * Переписываем тип на 0x0E (FAT16 LBA). Правка затрагивает один байт в MBR,
   * данные и файловую систему не трогает и повторное выполнение безвредно. */
  if (fs_ok && disk_read(0, work, 0, 1) == RES_OK)
  {
      if (work[MBR_PART_TYPE_OFS] != 0x0E)
      {
          work[MBR_PART_TYPE_OFS] = 0x0E;
          disk_write(0, work, 0, 1);
      }
  }

  // 2b) Архив начинается заново в двух случаях:
  //     — кнопка KEY удержана в момент старта (ручной сброс при битых данных).
  //       Жест требует физического сброса с зажатой кнопкой, поэтому в поле
  //       случайно не повторится: короткое нажатие в работе означает «разбудить»,
  //       а удержание в работе держит USB-сессию — оба с этим не пересекаются;
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

  if (fs_ok && (wipe_requested || header_mismatch))
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
          f_puts(CSV_HEADER "\r\n", &file);
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

	  /* Флеш — на постоянном питании, гасить её нечем, кроме Deep Power-Down.
	   * Делаем это только сейчас: пока кнопка удерживалась (USB-сессия выше),
	   * хост мог читать диск, и усыплять флеш было нельзя. */
	  SPI_Flash_DeepPowerDown();

	  /* Гасим периферию шин. Порядок важен: SPI деинициализируем строго ПОСЛЕ
	   * команды Deep Power-Down выше — иначе её нечем было бы отправить. */
	  HAL_SPI_DeInit(&hspi1);
	  HAL_I2C_DeInit(&hi2c1);

	  /* HAL_..._DeInit оставляет выводы «плавающими» входами, а не в аналоге,
	   * поэтому доводим их до тихого состояния вручную. */
	  MX_GPIO_SleepPrepare();

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

	  /* Возвращаем выводы 1-Wire из аналога и поднимаем SPI — обязательно до
	   * любого обращения к флеш, иначе команду пробуждения будет некому послать. */
	  MX_GPIO_SleepRestore();
	  MX_SPI1_Init();

	  /* Будим флеш до любого обращения к ней (в DPD она игнорирует все команды, кроме 0xAB). */
	  SPI_Flash_ReleasePowerDown();

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

	  /* Кнопка KEY: нажата = 0 (замкнута на GND). */
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

	  Battery_Data bat;
	  Battery_Read(&bat);

	  char ts[32];
	  Time_GetTimestamp(ts, sizeof(ts));

	  uint32_t t_sensors_ms = HAL_GetTick() - t_wake;   /* TEST-ONLY */

	  /* Если ФС не смонтирована (сбой при старте или отвалилась позже) — пробуем
	   * поднять её заново, но не чаще одного раза за цикл. Между попытками
	   * устройство спит, поэтому даже неустранимая поломка не сажает батарею. */
	  if (!fs_ok)
	      fs_ok = (f_mount(&USERFatFS, USERPath, 1) == FR_OK) ? 1U : 0U;

	  FIL file;
	  res = fs_ok ? f_open(&file, DATA_FILE, FA_OPEN_APPEND | FA_WRITE) : FR_NOT_READY;

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
	      char line[288];
	      /* TEST-ONLY: хвостовые поля — диагностика сбоев AHT20 и профиль времени цикла:
	       *   aht_ready_ms  — через сколько мс после подачи питания датчик ответил (9999 = не ответил)
	       *   t_sensors_ms  — время от пробуждения до записи (I2C + DS18B20 + ADC)
	       *   t_write_ms    — сколько заняла запись в флеш на ПРОШЛОМ цикле
	       * Убрать вместе с соответствующей логикой после отладки. */
	      snprintf(line, sizeof(line),
	               "%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%lu,%u,%lu,%lu,0x%02X,%u,%u,%u,%u,%lu,%lu,"
	               "%u,%.2f,%.2f,%u\r\n",
	               ts,
	               aht.temperature,
	               aht.humidity,
	               soil_temp,
	               soil_hum,
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
	               (unsigned long)last_hold_ms,
	               (unsigned long)write_fails,
	               (unsigned)bat.percent,
	               bat.volts,
	               bat.vdd,
	               /* soil_valid: ниже дропаута LDO опора АЦП уплывает вместе с банкой,
	                * поэтому влажность почвы с этого момента недостоверна. */
	               (unsigned)(bat.valid && !bat.low));

	      uint32_t t_w0 = HAL_GetTick();
	      int      puts_res  = f_puts(line, &file);
	      FRESULT  close_res = f_close(&file);
	      last_write_ms = HAL_GetTick() - t_w0;

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
	  else
	  {
	      write_fails++;
	      /* Заполненный архив — не поломка тома, перемонтирование тут не поможет
	       * и только зря потратит энергию на каждом цикле. */
	      if (res != FR_DENIED)
	          fs_ok = 0;   /* не открылось — пробуем перемонтировать в следующем цикле */
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
