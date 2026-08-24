#include "aht20.h"
#include "aht20_decode.h"
#include "stm32f4xx_hal.h"

#define AHT20_ADDR        (0x38 << 1)
#define AHT20_CMD_INIT    0xBE
#define AHT20_CMD_TRIGGER 0xAC
#define AHT20_CMD_RESET   0xBA   /* мягкий сброс, по даташиту не дольше 20 мс */
#define AHT20_STATUS_BUSY 0x80
#define AHT20_STATUS_CAL  0x08   /* бит "датчик откалиброван" в статусном байте */

HAL_StatusTypeDef AHT20_ReadStatusByte(I2C_HandleTypeDef *hi2c, uint8_t *status)
{
    return HAL_I2C_Master_Receive(hi2c, AHT20_ADDR, status, 1, 100);
}

HAL_StatusTypeDef AHT20_Init(I2C_HandleTypeDef *hi2c)
{
    uint8_t cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
    uint8_t status = 0;

    /* После потери питания датчик поднимается некалиброванным: он отвечает на шине,
     * транзакции проходят успешно, но выдаёт нули. Мало просто послать 0xBE —
     * нужно убедиться, что бит калибровки реально встал, и повторить при необходимости. */
    for (uint8_t attempt = 0; attempt < 5; attempt++)
    {
        if (AHT20_ReadStatusByte(hi2c, &status) == HAL_OK &&
            (status & AHT20_STATUS_CAL) != 0)
        {
            return HAL_OK;
        }

        /* Если команда инициализации не подействовала с первого раза, дело не в
         * ней: датчик поднялся в состоянии, из которого 0xBE его не выводит.
         * Штатный выход отсюда — мягкий сброс, он дешевле любой возни с
         * питанием. Снять питание толком нельзя: развязывающий конденсатор
         * модуля разряжать нечем, и на это ушли бы десятки секунд активной
         * фазы — половина всего бюджета тока. */
        if (attempt == 1)
        {
            uint8_t rst = AHT20_CMD_RESET;
            HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, &rst, 1, 100);
            HAL_Delay(20);
        }

        HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100);
        HAL_Delay(20);   /* датчику нужно время обработать команду инициализации */
    }

    return HAL_ERROR;   /* так и не откалибровался */
}

/* Один цикл "запустить измерение — дождаться — разобрать".
 * Вынесено отдельно, потому что повторять при испорченном кадре надо именно
 * его целиком: перечитывание по шине отдаёт те же самые байты, новый результат
 * появляется только после новой команды запуска. */
static AHT20_DecodeStatus AHT20_MeasureOnce(I2C_HandleTypeDef *hi2c,
                                            AHT20_Data *out, int *bus_ok)
{
    uint8_t cmd[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    uint8_t buf[6] = {0};
    HAL_StatusTypeDef status = HAL_ERROR;

    *bus_ok = 0;

    for (uint8_t attempt = 0; attempt < 3; attempt++)
    {
        status = HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100);
        if (status == HAL_OK)
            break;
        HAL_Delay(50);
    }
    if (status != HAL_OK)
        return AHT20_DEC_ERR_BUSY;   /* шина не отвечает — повторять бессмысленно */

    *bus_ok = 1;
    HAL_Delay(80);

    /* Пока установлен бит BUSY — измерение ещё не готово, остальным байтам
     * доверять нельзя. Дожидаемся с ограничением попыток. */
    for (uint8_t attempt = 0; ; attempt++)
    {
        if (HAL_I2C_Master_Receive(hi2c, AHT20_ADDR, buf, 6, 100) != HAL_OK)
        {
            *bus_ok = 0;
            return AHT20_DEC_ERR_BUSY;
        }

        AHT20_DecodeStatus dec = AHT20_DecodeFrame(buf, &out->temperature, &out->humidity);

        if (dec != AHT20_DEC_ERR_BUSY)
            return dec;              /* готово либо испорчено — ждать больше нечего */

        if (attempt >= 4)
            return AHT20_DEC_ERR_BUSY; /* так и не дождались завершения измерения */

        HAL_Delay(20);
    }
}

HAL_StatusTypeDef AHT20_ReadData(I2C_HandleTypeDef *hi2c, AHT20_Data *out)
{
    /* Две попытки, а не одна: испорченный кадр может быть разовым сбоем, и
     * тогда повторный запуск измерения возвращает нормальные данные. Раньше
     * такой кадр либо уходил в архив как -50.00 °C, либо (после первой
     * починки) просто терял замер.
     *
     * Платим только при отказе: около 200 мс, если кадр пришёл сразу и оказался
     * вне шкалы, и до ~0.8 с в худшем случае, когда датчик все пять раз отвечает
     * BUSY. Раз в час при токе активной фазы порядка 20 мА даже худший случай
     * стоит меньше 5 мкА среднего против бюджета в 377 мкА. */
    for (uint8_t round = 0; round < 2; round++)
    {
        int bus_ok = 0;
        AHT20_DecodeStatus dec = AHT20_MeasureOnce(hi2c, out, &bus_ok);

        if (dec == AHT20_DEC_OK)
            return HAL_OK;

        /* Шина молчит — повторять нечего, датчик обесточен или оторван. */
        if (!bus_ok)
            return HAL_ERROR;

        /* Сплошные нули означают некалиброванный датчик, а не незаконченное
         * измерение: новый запуск даст тот же пустой кадр, лечится это только
         * переинициализацией на следующем цикле. */
        if (dec == AHT20_DEC_ERR_ZERO)
            return HAL_ERROR;
    }

    return HAL_ERROR;
}


