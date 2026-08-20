/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.h
  * @brief   This file contains all the function prototypes for
  *          the adc.c file
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
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __ADC_H__
#define __ADC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern ADC_HandleTypeDef hadc1;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_ADC1_Init(void);

/* Одиночное измерение произвольного канала. Канал переконфигурируется перед
 * каждым замером — сканирование и DMA не используются, каналов всего три
 * (почва, батарея, VREFINT), и они читаются по очереди раз в цикл.
 * Возвращает 1 при успехе, 0 при ошибке или таймауте. */
uint8_t ADC_ReadChannel(uint32_t channel, uint32_t sampling_time, uint32_t *raw_out);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __ADC_H__ */

