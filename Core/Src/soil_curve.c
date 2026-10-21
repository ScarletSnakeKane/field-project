#include "soil_curve.h"
#include <math.h>

float Soil_VoltageToMoisture(float volts)
{
    /* Сначала окно правдоподобия, и только потом прижим: прижим превращает
     * обрыв провода в ровные 0 или 100 %, которые от настоящих не отличить.
     * Условие записано через отрицание, чтобы NAN на входе тоже ушёл в NAN. */
    if (!(volts >= SOIL_MIN_VALID_VOLTS && volts <= SOIL_MAX_VALID_VOLTS))
        return NAN;

    float moisture = (SOIL_AIR_VOLTS - volts) /
                     (SOIL_AIR_VOLTS - SOIL_WATER_VOLTS) * 100.0f;

    if (moisture < 0.0f)   moisture = 0.0f;
    if (moisture > 100.0f) moisture = 100.0f;

    return moisture;
}
