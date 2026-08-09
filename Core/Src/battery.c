#include "battery.h"

/* Заводская калибровка внутреннего опорного источника: значение VREFINT,
 * измеренное на заводе при VDDA = 3.30 В. Позволяет вычислить фактическое VDD,
 * а не полагаться на «наверное, 3.3». Для батарейного питания это принципиально:
 * когда банка просядет ниже дропаута LDO, VDD поедет вместе с ней, и замер
 * относительно предполагаемых 3.3 В начнёт врать ровно тогда, когда он нужнее. */
#define VREFINT_CAL_ADDR    ((uint16_t*)0x1FFF7A2AU)
#define VREFINT_CAL_VDDA    3.30f

#define BAT_ADC_CHANNEL     ADC_CHANNEL_2   /* PA2 */

/* Длинная выборка: источник высокоомный (при 100к+100к эквивалент ~50 кОм),
 * входной конденсатор АЦП должен успеть зарядиться до 12-битной точности. */
#define BAT_SAMPLE_TIME     ADC_SAMPLETIME_84CYCLES
/* VREFINT по даташиту требует не меньше 10 мкс выборки. */
#define VREF_SAMPLE_TIME    ADC_SAMPLETIME_144CYCLES

/* Кривая разряда Li-ion под малым током. Линейный пересчёт «вольты в проценты»
 * для этой химии даёт грубо неверную картину: почти вся ёмкость лежит в пологом
 * участке 3.9–3.6 В, а края шкалы проскакивают за считанные проценты ёмкости. */
static const struct { float v; uint8_t p; } bat_curve[] = {
    {4.20f, 100}, {4.10f, 95}, {4.00f, 87}, {3.90f, 77},
    {3.80f,  65}, {3.70f, 52}, {3.60f, 38}, {3.50f, 24},
    {3.40f,  12}, {3.30f,  6}, {3.20f,  3}, {3.00f,  0},
};

static uint8_t Battery_VoltsToPercent(float v)
{
    const uint8_t n = sizeof(bat_curve) / sizeof(bat_curve[0]);

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

uint8_t Battery_Read(Battery_Data *out)
{
    uint32_t raw_bat = 0, raw_ref = 0;

    out->valid   = 0;
    out->volts   = 0.0f;
    out->vdd     = 0.0f;
    out->percent = 0;
    out->low     = 0;

    if (!ADC_ReadChannel(BAT_ADC_CHANNEL, BAT_SAMPLE_TIME, &raw_bat))
        return 0;

    /* Первое измерение опоры холостое: HAL включает внутренний источник только
     * при выборе этого канала, а ему нужно порядка 10 мкс на запуск. Результат
     * первой выборки может оказаться снятым до выхода на режим. */
    if (!ADC_ReadChannel(ADC_CHANNEL_VREFINT, VREF_SAMPLE_TIME, &raw_ref))
        return 0;

    if (!ADC_ReadChannel(ADC_CHANNEL_VREFINT, VREF_SAMPLE_TIME, &raw_ref))
        return 0;

    if (raw_ref == 0)
        return 0;   /* защита от деления на ноль, если опорный канал не поднялся */

    float vdda = VREFINT_CAL_VDDA * (float)(*VREFINT_CAL_ADDR) / (float)raw_ref;

    out->vdd     = vdda;
    out->volts   = ((float)raw_bat / 4095.0f) * vdda * BAT_DIVIDER_RATIO;
    out->percent = Battery_VoltsToPercent(out->volts);
    out->low     = (out->volts < BAT_LOW_VOLTS) ? 1U : 0U;
    out->valid   = 1;

    return 1;
}
