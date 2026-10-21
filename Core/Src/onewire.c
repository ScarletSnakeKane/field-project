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

/* Чтение одного бита.
 *
 * DS18B20 гарантирует уровень на линии только 15 мкс от спада, и выборка
 * обязана уложиться в это окно. Прежняя схема «6 мкс + 9 мкс» через
 * HAL_GPIO_* и OneWire_Delay на 16 МГц с нулевой латентностью флеша
 * набирала накладных ~0.5-1 мкс на каждый вызов и делала выборку около
 * 17-18 мкс — уже за окном: на холоде и с длинным кабелем ноль читался
 * единицей (ds_err = 3, а изредка — совпавший по CRC ложный кадр).
 *
 * Теперь все моменты отсчитываются от одной метки DWT, взятой до спада,
 * а выводом управляем прямо через BSRR/IDR, без вызовов. Линию держим
 * 2 мкс (минимум по даташиту 1 мкс) и читаем на 12 мкс: с учётом шага
 * цикла ожидания (~0.4 мкс) выборка не позже ~12.5 мкс от спада, запас до
 * 15 мкс — больше 2 мкс. Ранний отпуск линии сохраняет единице те же
 * ~10 мкс на подъём через подтяжку, что и прежде, — читать раньше за счёт
 * длинного кабеля не пришлось. Длина слота прежняя, 70 мкс. Прерывания на
 * время слота запрещены, как и раньше. */
uint8_t OneWire_ReadBit(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
    uint8_t bit;
    const uint32_t tpus = SystemCoreClock / 1000000U;   /* считаем до спада, а не внутри окна */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t t0 = DWT->CYCCNT;
    GPIOx->BSRR = (uint32_t)GPIO_Pin << 16U;            /* линия вниз */
    while ((DWT->CYCCNT - t0) < 2U * tpus);

    GPIOx->BSRR = GPIO_Pin;                             /* отпустить линию */
    while ((DWT->CYCCNT - t0) < 12U * tpus);

    bit = ((GPIOx->IDR & GPIO_Pin) != 0U) ? 1U : 0U;

    while ((DWT->CYCCNT - t0) < 70U * tpus);            /* добить слот */

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
