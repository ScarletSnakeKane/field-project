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

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
FRESULT res;
static BYTE work[4096];
FIL file;
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
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == RESET)
  {
      // LSE НЕ запустился → RTC работает на LSI → время будет неправильным
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET); // просто маркер
  }
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */

  // Включаем питание датчиков (PA0 = LOW)
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);

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
          f_puts("timestamp,air_temp,air_hum,soil_temp,soil_hum\r\n", &file);
          f_close(&file);
      }
  }
  else if (res == FR_OK)
  {
      // Файл существует → просто закрываем
      f_close(&file);
  }

  // === Первое чтение и запись при старте ===
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
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
  while (1)
  {
	  HAL_Delay(5000);

	  uint8_t usb_state = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9);  // 0 или 1

	  AHT20_Data aht;
	  if (AHT20_ReadData(&hi2c1, &aht) != HAL_OK)
	  {
	      aht.temperature = 0.0f;
	      aht.humidity    = 0.0f;
	  }

	  float soil_temp;
	  	  if (!DS18B20_ReadTemp(GPIOA, GPIO_PIN_8, &soil_temp))
	  	      soil_temp = NAN; // датчик не ответил или CRC не сошёлся

	  float soil_hum  = Soil_ReadMoisture();

	  char ts[32];
	  Time_GetTimestamp(ts, sizeof(ts));

	  FIL file;
	  res = f_open(&file, "data.csv", FA_OPEN_APPEND | FA_WRITE);
	  if (res == FR_OK)
	  {
	      char line[160];
	      snprintf(line, sizeof(line),
	               "%s,%.2f,%.2f,%.2f,%.2f,%d\r\n",
	               ts,
	               aht.temperature,
	               aht.humidity,
	               soil_temp,
	               soil_hum,
	               usb_state);   // ← добавили состояние PA9

	      f_puts(line, &file);
	      f_close(&file);
	  }
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
