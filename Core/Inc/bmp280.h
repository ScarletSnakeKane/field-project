#ifndef BMP280_H
#define BMP280_H

#include "stm32f4xx_hal.h"

#define BMP280_ADDR          (0x77 << 1)

typedef struct {
    float temperature;
    float pressure;
} BMP280_Data;

HAL_StatusTypeDef BMP280_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef BMP280_ReadData(I2C_HandleTypeDef *hi2c, BMP280_Data *out);

#endif
