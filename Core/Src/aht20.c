#include "aht20.h"
#include "stm32f4xx_hal.h"

#define AHT20_ADDR        (0x38 << 1)
#define AHT20_CMD_INIT    0xBE
#define AHT20_CMD_TRIGGER 0xAC
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

        HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100);
        HAL_Delay(20);   /* датчику нужно время обработать команду инициализации */
    }

    return HAL_ERROR;   /* так и не откалибровался */
}

HAL_StatusTypeDef AHT20_ReadData(I2C_HandleTypeDef *hi2c, AHT20_Data *out)
{
    uint8_t cmd[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    uint8_t buf[6] = {0};
    HAL_StatusTypeDef status = HAL_ERROR;

    for (uint8_t attempt = 0; attempt < 3; attempt++)
    {
        status = HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100);
        if (status == HAL_OK)
            break;
        HAL_Delay(50);
    }
    if (status != HAL_OK)
        return HAL_ERROR;

    HAL_Delay(80);

    /* Пока установлен бит BUSY в статусном байте — измерение ещё не готово,
     * данные в остальных байтах нельзя доверять. Дожидаемся с ограничением попыток. */
    for (uint8_t attempt = 0; ; attempt++)
    {
        if (HAL_I2C_Master_Receive(hi2c, AHT20_ADDR, buf, 6, 100) != HAL_OK)
            return HAL_ERROR;

        if ((buf[0] & AHT20_STATUS_BUSY) == 0)
            break;

        if (attempt >= 4)
            return HAL_ERROR; /* так и не дождались завершения измерения */

        HAL_Delay(20);
    }

    uint32_t raw_hum = ((uint32_t)buf[1] << 12) |
                       ((uint32_t)buf[2] << 4)  |
                       ((uint32_t)buf[3] >> 4);

    uint32_t raw_temp = (((uint32_t)buf[3] & 0x0F) << 16) |
                        ((uint32_t)buf[4] << 8) |
                        ((uint32_t)buf[5]);

    out->humidity = (raw_hum * 100.0f) / 1048576.0f;
    out->temperature = (raw_temp * 200.0f / 1048576.0f) - 50.0f;

    return HAL_OK;
}


