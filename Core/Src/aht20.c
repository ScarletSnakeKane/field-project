#include "aht20.h"
#include "stm32f4xx_hal.h"

#define AHT20_ADDR        (0x38 << 1)
#define AHT20_CMD_INIT    0xBE
#define AHT20_CMD_TRIGGER 0xAC

HAL_StatusTypeDef AHT20_Init(I2C_HandleTypeDef *hi2c)
{
    uint8_t cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
    return HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100);
}

HAL_StatusTypeDef AHT20_ReadData(I2C_HandleTypeDef *hi2c, AHT20_Data *out)
{
    uint8_t cmd[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    uint8_t buf[6];

    if (HAL_I2C_Master_Transmit(hi2c, AHT20_ADDR, cmd, 3, 100) != HAL_OK)
        return HAL_ERROR;

    HAL_Delay(80);

    if (HAL_I2C_Master_Receive(hi2c, AHT20_ADDR, buf, 6, 100) != HAL_OK)
        return HAL_ERROR;

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


