#include "ds18b20.h"
#include "onewire.h"

static DS18B20_Status ds18b20_last_error = DS18B20_OK;

DS18B20_Status DS18B20_LastError(void)
{
    return ds18b20_last_error;
}

uint8_t DS18B20_ReadTemp(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, float *temp_out)
{
    uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN];

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

    for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; i++)
        scratchpad[i] = OneWire_ReadByte(GPIOx, GPIO_Pin);

    DS18B20_Status st = DS18B20_DecodeScratchpad(scratchpad, temp_out);
    if (st != DS18B20_OK)
    {
        ds18b20_last_error = st;
        return 0;
    }

    ds18b20_last_error = DS18B20_OK;
    return 1; // успех
}
