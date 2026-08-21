/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET); // 1-Wire в состоянии покоя = высокий уровень (отпущена)
    HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET); // питание датчиков включено

    /*Configure GPIO pin : PA4 (CS флешки) */
    GPIO_InitStruct.Pin = GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /*Configure GPIO pin : PB0 (питание датчиков через транзистор, HIGH = выключено) */
    GPIO_InitStruct.Pin = SENSOR_PWR_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SENSOR_PWR_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pin : PA0 (кнопка KEY, замыкает на GND) —
     * прерывание по спаду = момент нажатия, оно же будит МК из STOP. */
    GPIO_InitStruct.Pin = WAKE_BTN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(WAKE_BTN_GPIO_Port, &GPIO_InitStruct);

    /* Приоритет ниже, чем у RTC_WKUP (0,0) — пробуждение по кнопке не должно
     * вклиниваться в тайминги 1-Wire агрессивнее, чем уже делает RTC. */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    /*Configure GPIO pin : PA9 (делитель с VBUS) — прерывание по фронту:
     * подключили кабель, значит нужно проснуться и не спать, пока он воткнут.
     * Спад не ловим: пока VBUS высокий, МК и так не уходит в STOP, поэтому
     * отключение кабеля происходит только в бодрствующем состоянии.
     * Подтяжка отключена — её роль выполняет нижнее плечо делителя. */
    GPIO_InitStruct.Pin = VBUS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VBUS_GPIO_Port, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    /*Configure GPIO pin : PA8 (1-Wire шина DS18B20) — open-drain! */
    GPIO_InitStruct.Pin = GPIO_PIN_8;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;      // внутренняя подтяжка как подстраховка, но лучше иметь внешний резистор 4.7кОм
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* Все незадействованные выводы — в аналоговый режим. В нём отключён входной
     * триггер, поэтому «плавающий» уровень на ноге не вызывает сквозного тока;
     * для питания от батареи это штатная мера. Действует постоянно, не только во сне.
     * SWD (PA13/PA14) намеренно оставлены живыми — иначе отладчик перестанет
     * подключаться без аппаратного сброса. */
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;

    GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_10|GPIO_PIN_15;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5
                        |GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12
                        |GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4
                        |GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9
                        |GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

void MX_GPIO_SleepPrepare(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* PA8 (1-Wire) и PB8/PB9 (I2C) — в аналог без подтяжек.
   * Это главная утечка периода сна: датчики обесточены, но внутренние подтяжки
   * МК продолжают гнать ток в мёртвую рельсу — на I2C через подтяжки модуля,
   * на 1-Wire через защитный диод DQ->VDD обесточенного DS18B20. Аналоговый
   * режим заодно отключает входной триггер, который на «плавающем» уровне
   * потребляет сквозной ток. */
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  GPIO_InitStruct.Pin = GPIO_PIN_8;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* PA6 (MISO) — выход флеш, при CS=HIGH он в высокоимпедансном состоянии,
   * поэтому наш вход оставлять «плавающим» нельзя. */
  GPIO_InitStruct.Pin = GPIO_PIN_6;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PA5 (SCK) и PA7 (MOSI) — входы флеш, а она остаётся под питанием.
   * Их нельзя бросать «плавающими»: входной каскад флеш на промежуточном
   * уровне сам начнёт потреблять. Держим жёсткий ноль. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5|GPIO_PIN_7, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PA4 (CS) специально не трогаем — он должен остаться выходом в HIGH,
   * иначе флеш окажется выбранной и не уйдёт/не останется в Deep Power-Down. */
}

void MX_GPIO_SleepRestore(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* 1-Wire обратно в open-drain с подтяжкой; уровень покоя — высокий. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PA5/PA6/PA7 вернёт MX_SPI1_Init(), PB8/PB9 — MX_I2C1_Init(). */
}

/* USER CODE END 2 */
