#include "onewire.h"

// Микрозадержка (используем HAL)
void OneWire_Delay(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000);

    while ((DWT->CYCCNT - start) < ticks);
}

/* Тайминги 1-Wire меряются десятками микросекунд, а прерывания RTC (приоритет 0)
 * и кнопки могут вклиниться в любой момент и растянуть паузу — бит будет прочитан
 * или выдан неверно. Поэтому каждая элементарная операция выполняется атомарно.
 * Критические секции короткие (максимум ~1 мс на сброс шины), так что задержка
 * обработки прерываний некритична. */

// Сброс шины 1-Wire
uint8_t OneWire_Reset(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
    uint8_t presence;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    // Линия вниз на 480 мкс
    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_RESET);
    OneWire_Delay(480);

    // Линия вверх
    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_SET);
    OneWire_Delay(70);

    // Читаем presence
    presence = HAL_GPIO_ReadPin(GPIOx, GPIO_Pin);

    OneWire_Delay(410);

    __set_PRIMASK(primask);
    return presence == GPIO_PIN_RESET; // 0 = OK, 1 = NO DEVICE
}

// Запись одного бита
void OneWire_WriteBit(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, uint8_t bit)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_RESET);
    OneWire_Delay(bit ? 6 : 60);

    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_SET);
    OneWire_Delay(bit ? 64 : 10);

    __set_PRIMASK(primask);
}

// Чтение одного бита
uint8_t OneWire_ReadBit(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
    uint8_t bit;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_RESET);
    OneWire_Delay(6);

    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, GPIO_PIN_SET);
    OneWire_Delay(9);

    bit = HAL_GPIO_ReadPin(GPIOx, GPIO_Pin);

    OneWire_Delay(55);

    __set_PRIMASK(primask);
    return bit;
}

// Запись байта
void OneWire_WriteByte(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        OneWire_WriteBit(GPIOx, GPIO_Pin, byte & 0x01);
        byte >>= 1;
    }
}

// Чтение байта
uint8_t OneWire_ReadByte(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
    uint8_t byte = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        byte >>= 1;
        if (OneWire_ReadBit(GPIOx, GPIO_Pin))
            byte |= 0x80;
    }

    return byte;
}
