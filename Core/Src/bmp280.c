/* BMP280 — сверка температуры воздуха с AHT20 (решение владельца).
 *
 * Прежняя версия модуля не вызывалась и была непригодна к включению (П2):
 * Init не проверял коды возврата I2C, калибровка читалась без проверки, и
 * нулевые dig_T* давали ровно 0.00 °C; режим был normal — непрерывные
 * измерения и лишний ток. Теперь: каждый вызов I2C проверяется, калибровка
 * отвергается по Bmp280_CalibOk, а пересчёт — Bmp280_CompensateT с окном
 * правдоподобия (sensor_policy.h, под тестами). Один замер на цикл в forced
 * mode, давление не измеряется вовсе. */
#include "bmp280.h"
#include "sensor_policy.h"
#include <math.h>

#define BMP280_REG_CALIB     0x88
#define BMP280_REG_ID        0xD0
#define BMP280_REG_STATUS    0xF3
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_TEMP_MSB  0xFA

#define BMP280_STATUS_MEASURING  0x08U

/* ctrl_meas: osrs_t = x1, osrs_p = пропуск, режим forced. */
#define BMP280_CTRL_T1_FORCED    ((1U << 5) | (0U << 2) | 1U)

/* Замер температуры x1 по даташиту — до 5.5 мс; ждём с запасом. */
#define BMP280_MEAS_TIMEOUT_MS   20U

#define BMP280_I2C_TIMEOUT_MS    100U

static uint16_t bmp_addr;
static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;

static HAL_StatusTypeDef BMP280_ReadReg(I2C_HandleTypeDef *hi2c, uint16_t addr, uint8_t reg,
                                        uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(hi2c, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, BMP280_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef BMP280_Init(I2C_HandleTypeDef *hi2c)
{
    static const uint16_t addrs[2] = { BMP280_ADDR_LOW, BMP280_ADDR_HIGH };
    uint8_t id = 0;

    bmp_addr = 0;
    for (uint32_t i = 0; i < 2U; i++)
    {
        if (BMP280_ReadReg(hi2c, addrs[i], BMP280_REG_ID, &id, 1) == HAL_OK && id == BMP280_CHIP_ID)
        {
            bmp_addr = addrs[i];
            break;
        }
    }
    if (bmp_addr == 0U)
        return HAL_ERROR;

    uint8_t calib[6];
    if (BMP280_ReadReg(hi2c, bmp_addr, BMP280_REG_CALIB, calib, sizeof(calib)) != HAL_OK)
        return HAL_ERROR;

    dig_T1 = (uint16_t)(calib[1] << 8 | calib[0]);
    dig_T2 = (int16_t)(calib[3] << 8 | calib[2]);
    dig_T3 = (int16_t)(calib[5] << 8 | calib[4]);

    return Bmp280_CalibOk(dig_T1, dig_T2, dig_T3) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef BMP280_ReadTemperature(I2C_HandleTypeDef *hi2c, float *t_c)
{
    uint8_t ctrl = BMP280_CTRL_T1_FORCED;
    uint8_t st = 0;
    uint8_t raw[3];

    *t_c = NAN;
    if (bmp_addr == 0U)
        return HAL_ERROR;

    if (HAL_I2C_Mem_Write(hi2c, bmp_addr, BMP280_REG_CTRL_MEAS, I2C_MEMADD_SIZE_8BIT,
                          &ctrl, 1, BMP280_I2C_TIMEOUT_MS) != HAL_OK)
        return HAL_ERROR;

    /* Бит measuring поднимается не сразу после записи режима — первая
     * проверка через миллисекунду, иначе прочли бы прошлый (сбросовый)
     * отсчёт как готовый. */
    uint32_t t0 = HAL_GetTick();
    do
    {
        HAL_Delay(1);
        if (BMP280_ReadReg(hi2c, bmp_addr, BMP280_REG_STATUS, &st, 1) != HAL_OK)
            return HAL_ERROR;
        if ((st & BMP280_STATUS_MEASURING) == 0U)
            break;
    } while (HAL_GetTick() - t0 < BMP280_MEAS_TIMEOUT_MS);
    if (st & BMP280_STATUS_MEASURING)
        return HAL_TIMEOUT;

    if (BMP280_ReadReg(hi2c, bmp_addr, BMP280_REG_TEMP_MSB, raw, sizeof(raw)) != HAL_OK)
        return HAL_ERROR;

    int32_t adc_t = (int32_t)(((uint32_t)raw[0] << 12) | ((uint32_t)raw[1] << 4) | (raw[2] >> 4));
    *t_c = Bmp280_CompensateT(adc_t, dig_T1, dig_T2, dig_T3);
    return isnan(*t_c) ? HAL_ERROR : HAL_OK;
}
