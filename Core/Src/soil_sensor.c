#include "soil_sensor.h"

#define SOIL_VREF    3.3f      // опорное напряжение АЦП
#define AIR_VALUE    2.67465f  // напряжение при сухой почве
#define WATER_VALUE  0.87597f  // напряжение при влажной почве

float Soil_ReadVoltage(void)
{
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    return (raw * SOIL_VREF) / 4095.0f;  // перевод в вольты
}

float Soil_ReadMoisture(void)
{
    float v = Soil_ReadVoltage();
    float moisture = (AIR_VALUE - v) / (AIR_VALUE - WATER_VALUE) * 100.0f;

    if (moisture < 0) moisture = 0;
    if (moisture > 100) moisture = 100;

    return moisture;
}
