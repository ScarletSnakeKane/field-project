#include "ds18b20.h"
#include "onewire.h"

static uint8_t DS18B20_CRC8(const uint8_t *data, uint8_t len)
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

static DS18B20_Status ds18b20_last_error = DS18B20_OK;

DS18B20_Status DS18B20_LastError(void)
{
    return ds18b20_last_error;
}

uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out)
{
    uint8_t scratchpad[9];
    int16_t raw;

    // Запуск измерения
    if (!OneWire_Reset(GPIOx, GPIO_Pin))
    {
        ds18b20_last_error = DS18B20_ERR_PRESENCE_1;
        return 0; // датчик не ответил presence-импульсом
    }

    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xCC); // SKIP ROM
    OneWire_WriteByte(GPIOx, GPIO_Pin, 0x44); // CONVERT T

    HAL_Delay(750); // ожидание завершения измерения

    // Чтение результата
    if (!OneWire_Reset(GPIOx, GPIO_Pin))
    {
        ds18b20_last_error = DS18B20_ERR_PRESENCE_2;
        return 0;
    }

    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xCC); // SKIP ROM
    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xBE); // READ SCRATCHPAD

    uint8_t any_nonzero = 0;
    for (uint8_t i = 0; i < 9; i++)
    {
        scratchpad[i] = OneWire_ReadByte(GPIOx, GPIO_Pin);
        if (scratchpad[i] != 0)
            any_nonzero = 1;
    }

    /* Сплошные нули проходят проверку CRC (CRC8 от нулей = 0), поэтому "мёртвая"
     * шина без этой проверки выглядела бы как успешное измерение 0.00 °C —
     * а для почвы 0 °C вполне правдоподобное значение, и подмена осталась бы незамеченной. */
    if (!any_nonzero)
    {
        ds18b20_last_error = DS18B20_ERR_ALL_ZERO;
        return 0;
    }

    if (DS18B20_CRC8(scratchpad, 8) != scratchpad[8])
    {
        ds18b20_last_error = DS18B20_ERR_CRC;
        return 0; // CRC не сошёлся — данные битые, отбрасываем
    }

    raw = (scratchpad[1] << 8) | scratchpad[0];
    *temp_out = raw / 16.0f;

    ds18b20_last_error = DS18B20_OK;
    return 1; // успех
}
