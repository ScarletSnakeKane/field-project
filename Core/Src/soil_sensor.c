#include "soil_sensor.h"
#include <math.h>

#define SOIL_VREF    3.3f      // опорное напряжение АЦП
#define AIR_VALUE    2.67465f  // напряжение при сухой почве
#define WATER_VALUE  0.87597f  // напряжение при влажной почве

#define SOIL_ADC_CHANNEL     ADC_CHANNEL_1        /* PA1 */
#define SOIL_SAMPLE_TIME     ADC_SAMPLETIME_84CYCLES

float Soil_ReadVoltage(void)
{
    uint32_t raw;

    /* Канал задаём явно: тот же АЦП читает ещё батарею и VREFINT,
     * поэтому полагаться на конфигурацию, оставшуюся от прошлого замера, нельзя. */
    if (!ADC_ReadChannel(SOIL_ADC_CHANNEL, SOIL_SAMPLE_TIME, &raw))
        return NAN;

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
