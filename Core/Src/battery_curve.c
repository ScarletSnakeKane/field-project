#include "battery_curve.h"
#include <math.h>

/* Кривая разряда Li-ion под малым током. */
static const struct { float v; uint8_t p; } bat_curve[] = {
    {4.20f, 100}, {4.10f, 95}, {4.00f, 87}, {3.90f, 77},
    {3.80f,  65}, {3.70f, 52}, {3.60f, 38}, {3.50f, 24},
    {3.40f,  12}, {3.30f,  6}, {3.20f,  3}, {3.00f,  0},
};

uint8_t Battery_VoltsToPercent(float v)
{
    const uint8_t n = sizeof(bat_curve) / sizeof(bat_curve[0]);

    /* NAN проскакивал все сравнения ниже и становился 0 % — «батарея пуста». */
    if (isnan(v))
        return BAT_PERCENT_UNKNOWN;

    if (v >= bat_curve[0].v)
        return 100;
    if (v <= bat_curve[n - 1].v)
        return 0;

    for (uint8_t i = 1; i < n; i++)
    {
        if (v >= bat_curve[i].v)
        {
            /* линейная интерполяция внутри отрезка кривой */
            float span = bat_curve[i - 1].v - bat_curve[i].v;
            float pos  = v - bat_curve[i].v;
            float p    = bat_curve[i].p +
                         (bat_curve[i - 1].p - bat_curve[i].p) * (pos / span);
            return (uint8_t)(p + 0.5f);
        }
    }

    return 0;
}

uint8_t Battery_ComputeVolts(uint32_t raw_bat, uint32_t raw_ref, uint16_t vrefint_cal,
                             float *vdd_out, float *volts_out)
{
    *vdd_out   = NAN;
    *volts_out = NAN;

    /* Ноль опоры — канал не поднялся, делить на него нельзя. Насыщенный канал
     * батареи значит лишь «не меньше шкалы», настоящее напряжение неизвестно. */
    if (raw_ref == 0 || raw_bat >= 4095U)
        return 0;

    float vdd   = VREFINT_CAL_VDDA * (float)vrefint_cal / (float)raw_ref;
    float volts = ((float)raw_bat / 4095.0f) * vdd * BAT_DIVIDER_RATIO;

    if (!(vdd >= BAT_VDD_MIN && vdd <= BAT_VDD_MAX))
        return 0;
    if (!(volts >= BAT_VOLTS_MIN && volts <= BAT_VOLTS_MAX))
        return 0;

    *vdd_out   = vdd;
    *volts_out = volts;
    return 1;
}
