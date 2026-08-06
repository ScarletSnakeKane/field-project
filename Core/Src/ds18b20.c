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

uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out)
{
    uint8_t scratchpad[9];
    int16_t raw;

    // Запуск измерения
    if (!OneWire_Reset(GPIOx, GPIO_Pin))
        return 0; // датчик не ответил presence-импульсом

    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xCC); // SKIP ROM
    OneWire_WriteByte(GPIOx, GPIO_Pin, 0x44); // CONVERT T

    HAL_Delay(750); // ожидание завершения измерения

    // Чтение результата
    if (!OneWire_Reset(GPIOx, GPIO_Pin))
        return 0;

    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xCC); // SKIP ROM
    OneWire_WriteByte(GPIOx, GPIO_Pin, 0xBE); // READ SCRATCHPAD

    for (uint8_t i = 0; i < 9; i++)
        scratchpad[i] = OneWire_ReadByte(GPIOx, GPIO_Pin);

    if (DS18B20_CRC8(scratchpad, 8) != scratchpad[8])
        return 0; // CRC не сошёлся — данные битые, отбрасываем

    raw = (scratchpad[1] << 8) | scratchpad[0];
    *temp_out = raw / 16.0f;

    return 1; // успех
}
