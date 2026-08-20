#ifndef __DS18B20_DECODE_H__
#define __DS18B20_DECODE_H__

/* Разбор ответа DS18B20, отделённый от работы с шиной.
 *
 * Здесь намеренно нет ни HAL, ни GPIO: всё, что делает этот модуль — превращает
 * девять прочитанных байт в температуру или в причину отказа. Благодаря этому
 * его можно прогонять тестами на обычном компьютере, а именно в разборе и
 * пряталась самая неприятная ошибка проекта (см. DS18B20_ERR_ALL_ZERO). */

#include <stdint.h>

/* Причина последнего неудачного чтения (для диагностики) */
typedef enum {
    DS18B20_OK              = 0,
    DS18B20_ERR_PRESENCE_1  = 1,  // нет presence-импульса перед запуском измерения
    DS18B20_ERR_PRESENCE_2  = 2,  // нет presence-импульса перед чтением результата
    DS18B20_ERR_CRC         = 3,  // CRC не сошёлся
    DS18B20_ERR_ALL_ZERO    = 4,  // шина читается сплошными нулями (нет подтяжки/датчика)
} DS18B20_Status;

#define DS18B20_SCRATCHPAD_LEN   9

/* CRC-8 в варианте Dallas/Maxim (полином X^8 + X^5 + X^4 + 1, отражённый 0x8C). */
uint8_t DS18B20_CRC8(const uint8_t *data, uint8_t len);

/* Превращает scratchpad в температуру.
 * Возвращает DS18B20_OK и заполняет temp_out либо причину отказа. */
DS18B20_Status DS18B20_DecodeScratchpad(const uint8_t *sp, float *temp_out);

#endif
