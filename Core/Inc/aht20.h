#ifndef AHT20_H
#define AHT20_H

#include "stm32f4xx_hal.h"

#define AHT20_ADDR        (0x38 << 1)

typedef struct {
    float temperature;
    float humidity;
} AHT20_Data;

HAL_StatusTypeDef AHT20_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef AHT20_ReadData(I2C_HandleTypeDef *hi2c, AHT20_Data *out);

#endif
