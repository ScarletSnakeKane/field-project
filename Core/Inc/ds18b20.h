#ifndef __DS18B20_H__
#define __DS18B20_H__

#include "stm32f4xx_hal.h"

// Чтение температуры с DS18B20
uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out);

#endif
