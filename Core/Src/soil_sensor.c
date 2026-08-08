#include "soil_sensor.h"
#include <math.h>

#define SOIL_VREF    3.3f      // опорное напряжение АЦП
#define AIR_VALUE    2.67465f  // напряжение при сухой почве
#define WATER_VALUE  0.87597f  // напряжение при влажной почве

#define SOIL_ADC_TIMEOUT_MS  10U

float Soil_ReadVoltage(void)
{
    /* Конечный таймаут вместо HAL_MAX_DELAY: одиночное преобразование занимает
     * микросекунды, а бесконечное ожидание в поле означало бы зависание навсегда. */
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
        return NAN;

    if (HAL_ADC_PollForConversion(&hadc1, SOIL_ADC_TIMEOUT_MS) != HAL_OK)
    {
        HAL_ADC_Stop(&hadc1);
        return NAN;
    }

    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    return (raw * SOIL_VREF) / 4095.0f;  // перевод в вольты
}

float Soil_ReadMoisture(void)
{
    float v = Soil_ReadVoltage();
    if (isnan(v))
        return NAN;   // АЦП не ответил — честнее отдать NAN, чем 0% влажности

    float moisture = (AIR_VALUE - v) / (AIR_VALUE - WATER_VALUE) * 100.0f;

    if (moisture < 0) moisture = 0;
    if (moisture > 100) moisture = 100;

    return moisture;
}
