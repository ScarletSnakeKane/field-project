/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.h
  * @brief   This file contains all the function prototypes for
  *          the rtc.c file
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
#ifndef __RTC_H__
#define __RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern RTC_HandleTypeDef hrtc;

/* USER CODE BEGIN Private defines */

/* Интервал между замерами, в секундах (RTC_WAKEUPCLOCK_CK_SPRE_16BITS = тики по 1 с).
 *
 * Боевое значение: раз в час. Счётчик 16-битный, так что верхний предел этого
 * режима — 65535 с, и запас ещё есть.
 *
 * Внимание при отладке: при часовом интервале плата бодрствует полторы секунды
 * из трёх с половиной тысяч, а в STOP отладочный порт обесточен — попасть в
 * неё программатором вслепую практически невозможно. Чтобы прошить или снять
 * данные, нажмите и удерживайте KEY: нажатие будит плату из STOP, а удержание
 * не даёт ей заснуть снова. Короткие значения (15-30 с) удобны для стенда, но
 * в поле дают среднее потребление на порядки выше — вся экономия проекта
 * держится на том, что прибор спит почти всё время. */
#define RTC_WAKEUP_INTERVAL_SEC   3600U

/* USER CODE END Private defines */

void MX_RTC_Init(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H__ */

