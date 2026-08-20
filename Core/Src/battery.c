#include "battery.h"
#include "battery_curve.h"

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
