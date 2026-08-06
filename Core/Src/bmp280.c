#include "bmp280.h"

#define BMP280_REG_ID        0xD0
#define BMP280_REG_RESET     0xE0
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG    0xF5
#define BMP280_REG_CALIB     0x88
#define BMP280_REG_PRESS_MSB 0xF7

static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

static int32_t t_fine;

static void BMP280_ReadCalibration(I2C_HandleTypeDef *hi2c)
{
    uint8_t calib[24];
    HAL_I2C_Mem_Read(hi2c, BMP280_ADDR, BMP280_REG_CALIB, 1, calib, 24, 100);

    dig_T1 = (uint16_t)(calib[1] << 8 | calib[0]);
    dig_T2 = (int16_t)(calib[3] << 8 | calib[2]);
    dig_T3 = (int16_t)(calib[5] << 8 | calib[4]);

    dig_P1 = (uint16_t)(calib[7] << 8 | calib[6]);
    dig_P2 = (int16_t)(calib[9] << 8 | calib[8]);
    dig_P3 = (int16_t)(calib[11] << 8 | calib[10]);
    dig_P4 = (int16_t)(calib[13] << 8 | calib[12]);
    dig_P5 = (int16_t)(calib[15] << 8 | calib[14]);
    dig_P6 = (int16_t)(calib[17] << 8 | calib[16]);
    dig_P7 = (int16_t)(calib[19] << 8 | calib[18]);
    dig_P8 = (int16_t)(calib[21] << 8 | calib[20]);
    dig_P9 = (int16_t)(calib[23] << 8 | calib[22]);
}

HAL_StatusTypeDef BMP280_Init(I2C_HandleTypeDef *hi2c)
{
    uint8_t id;
    HAL_I2C_Mem_Read(hi2c, BMP280_ADDR, BMP280_REG_ID, 1, &id, 1, 100);

    if (id != 0x58)
        return HAL_ERROR;

    uint8_t reset_cmd = 0xB6;
    HAL_I2C_Mem_Write(hi2c, BMP280_ADDR, BMP280_REG_RESET, 1, &reset_cmd, 1, 100);
    HAL_Delay(10);

    BMP280_ReadCalibration(hi2c);

    uint8_t ctrl_meas = 0x27; // temp x1, press x1, normal mode
    uint8_t config     = 0xA0; // 1000 ms standby

    HAL_I2C_Mem_Write(hi2c, BMP280_ADDR, BMP280_REG_CTRL_MEAS, 1, &ctrl_meas, 1, 100);
    HAL_I2C_Mem_Write(hi2c, BMP280_ADDR, BMP280_REG_CONFIG, 1, &config, 1, 100);

    return HAL_OK;
}

HAL_StatusTypeDef BMP280_ReadData(I2C_HandleTypeDef *hi2c, BMP280_Data *out)
{
    uint8_t buf[6];

    if (HAL_I2C_Mem_Read(hi2c, BMP280_ADDR, BMP280_REG_PRESS_MSB, 1, buf, 6, 100) != HAL_OK)
        return HAL_ERROR;

    int32_t adc_P = (int32_t)((buf[0] << 12) | (buf[1] << 4) | (buf[2] >> 4));
    int32_t adc_T = (int32_t)((buf[3] << 12) | (buf[4] << 4) | (buf[5] >> 4));

    // Temperature compensation
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
                      ((int32_t)dig_T3)) >> 14;

    t_fine = var1 + var2;
    out->temperature = (t_fine * 5 + 128) >> 8;
    out->temperature /= 100.0f;

    // Pressure compensation
    int64_t pvar1 = ((int64_t)t_fine) - 128000;
    int64_t pvar2 = pvar1 * pvar1 * (int64_t)dig_P6;
    pvar2 = pvar2 + ((pvar1 * (int64_t)dig_P5) << 17);
    pvar2 = pvar2 + (((int64_t)dig_P4) << 35);
    pvar1 = ((pvar1 * pvar1 * (int64_t)dig_P3) >> 8) +
            ((pvar1 * (int64_t)dig_P2) << 12);
    pvar1 = (((((int64_t)1) << 47) + pvar1)) * ((int64_t)dig_P1) >> 33;

    if (pvar1 == 0)
        return HAL_ERROR;

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - pvar2) * 3125) / pvar1;
    pvar1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    pvar2 = (((int64_t)dig_P8) * p) >> 19;

    p = ((p + pvar1 + pvar2) >> 8);
    out->pressure = p / 100.0f; // hPa

    return HAL_OK;
}
