#ifndef BMP280_H
#define BMP280_H

#include "stm32f4xx_hal.h"

/* BMP280 на модуле AHT20+BMP280 — только для сверки температуры воздуха
 * (решение владельца; правило — sensor_policy.h). Адрес зависит от перемычки
 * SDO на модуле (0x76 или 0x77), ответ схемотехника ещё не получен, поэтому
 * Init сам ищет датчик по обоим адресам по идентификатору 0x58. */
#define BMP280_ADDR_LOW   (0x76 << 1)
#define BMP280_ADDR_HIGH  (0x77 << 1)

/* Найти датчик и прочитать калибровку. Вызывать после каждой подачи питания:
 * рельса датчиков снимается на весь сон. HAL_ERROR — нет датчика, шина не
 * ответила или калибровка непригодна. */
HAL_StatusTypeDef BMP280_Init(I2C_HandleTypeDef *hi2c);

/* Один замер в forced mode — только температура, давление выключено.
 * После него датчик сам уходит в sleep. NAN в *t_c при любой неудаче. */
HAL_StatusTypeDef BMP280_ReadTemperature(I2C_HandleTypeDef *hi2c, float *t_c);

#endif
