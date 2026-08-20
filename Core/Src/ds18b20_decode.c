#include "ds18b20_decode.h"

uint8_t DS18B20_CRC8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t inbyte = data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix)
                crc ^= 0x8C;
            inbyte >>= 1;
        }
    }
    return crc;
}

DS18B20_Status DS18B20_DecodeScratchpad(const uint8_t *sp, float *temp_out)
{
    uint8_t any_nonzero = 0;
    for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; i++)
    {
        if (sp[i] != 0)
        {
            any_nonzero = 1;
            break;
        }
    }

    /* Сплошные нули проходят проверку CRC (CRC8 от нулей = 0), поэтому «мёртвая»
     * шина без этой проверки выглядела бы как успешное измерение 0.00 °C — а для
     * почвы 0 °C вполне правдоподобное значение, и подмена осталась бы незамеченной.
     * Проверка обязана стоять ДО CRC, иначе она никогда не сработает. */
    if (!any_nonzero)
        return DS18B20_ERR_ALL_ZERO;

    if (DS18B20_CRC8(sp, DS18B20_SCRATCHPAD_LEN - 1) != sp[DS18B20_SCRATCHPAD_LEN - 1])
        return DS18B20_ERR_CRC;

    /* Температура лежит в первых двух байтах как знаковое число в шагах 1/16 °C.
     * Приведение к int16_t обязано быть явным: без него отрицательные температуры
     * (старшие биты — единицы) превратились бы в +4000 °C с копейками. */
    int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | (uint16_t)sp[0]);
    *temp_out = (float)raw / 16.0f;

    return DS18B20_OK;
}
