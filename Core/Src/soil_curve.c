#include "soil_curve.h"

float Soil_VoltageToMoisture(float volts)
{
    float moisture = (SOIL_AIR_VOLTS - volts) /
                     (SOIL_AIR_VOLTS - SOIL_WATER_VOLTS) * 100.0f;

    if (moisture < 0.0f)   moisture = 0.0f;
    if (moisture > 100.0f) moisture = 100.0f;

    return moisture;
}
