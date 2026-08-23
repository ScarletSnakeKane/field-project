#include "aht20_decode.h"

#define AHT20_STATUS_BUSY   0x80

AHT20_DecodeStatus AHT20_DecodeFrame(const uint8_t *buf,
                                     float *temp_out, float *hum_out)
{
    if ((buf[0] & AHT20_STATUS_BUSY) != 0)
        return AHT20_DEC_ERR_BUSY;

    uint32_t raw_hum = ((uint32_t)buf[1] << 12) |
                       ((uint32_t)buf[2] << 4)  |
                       ((uint32_t)buf[3] >> 4);

    uint32_t raw_temp = (((uint32_t)buf[3] & 0x0F) << 16) |
                        ((uint32_t)buf[4] << 8) |
                        ((uint32_t)buf[5]);

    /* Оба поля в нуле разом — это не измерение, а некалиброванный датчик.
     * Влажность ровно 0 % физически не встречается даже в сухом воздухе, а
     * температура из нулей даёт ровно -50.00 °C: нижний край шкалы, то есть
     * значение, которое датчик не может выдать как результат измерения.
     * Проверять надо именно ОБА поля: сами по себе нули в одном из них
     * законны у краёв диапазона. */
    if (raw_temp == 0 && raw_hum == 0)
        return AHT20_DEC_ERR_ZERO;

    *hum_out  = (raw_hum * 100.0f) / 1048576.0f;
    *temp_out = (raw_temp * 200.0f / 1048576.0f) - 50.0f;

    return AHT20_DEC_OK;
}
