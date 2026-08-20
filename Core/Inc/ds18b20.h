#ifndef __DS18B20_H__
#define __DS18B20_H__

#include "stm32f4xx_hal.h"

/* Коды ошибок и разбор scratchpad живут в ds18b20_decode.h — там нет HAL,
 * поэтому их можно прогонять тестами на обычном компьютере. */
#include "ds18b20_decode.h"

// Чтение температуры с DS18B20
uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out);

// Код ошибки последнего вызова DS18B20_ReadTemp
DS18B20_Status DS18B20_LastError(void);

#endif
