#ifndef __DS18B20_H__
#define __DS18B20_H__

#include "stm32f4xx_hal.h"

// Причина последнего неудачного чтения (для диагностики)
typedef enum {
    DS18B20_OK              = 0,
    DS18B20_ERR_PRESENCE_1  = 1,  // нет presence-импульса перед запуском измерения
    DS18B20_ERR_PRESENCE_2  = 2,  // нет presence-импульса перед чтением результата
    DS18B20_ERR_CRC         = 3,  // CRC не сошёлся
    DS18B20_ERR_ALL_ZERO    = 4,  // шина читается сплошными нулями (нет подтяжки/датчика)
} DS18B20_Status;

// Чтение температуры с DS18B20
uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out);

// Код ошибки последнего вызова DS18B20_ReadTemp
DS18B20_Status DS18B20_LastError(void);

#endif
