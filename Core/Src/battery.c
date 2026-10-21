#include "battery.h"
#include "battery_curve.h"
#include <math.h>

/* Заводская калибровка внутреннего опорного источника: значение VREFINT,
 * измеренное на заводе при VDDA = 3.30 В. Позволяет вычислить фактическое VDD,
 * а не полагаться на «наверное, 3.3». Для батарейного питания это принципиально:
 * когда банка просядет ниже дропаута LDO, VDD поедет вместе с ней, и замер
 * относительно предполагаемых 3.3 В начнёт врать ровно тогда, когда он нужнее. */
/* Адрес заводской калибровки VREFINT берём из HAL (stm32f4xx_ll_adc.h), а не
 * дублируем своим define: значения совпадают, но собственная копия молча
 * разошлась бы с драйвером при смене семейства МК. Напряжение калибровки
 * (VREFINT_CAL_VDDA) и сам пересчёт живут в battery_curve — там их проверяют
 * тесты. */

#define BAT_ADC_CHANNEL     ADC_CHANNEL_2   /* PA2 */

/* Длинная выборка: источник высокоомный (при 100к+100к эквивалент ~50 кОм),
 * входной конденсатор АЦП должен успеть зарядиться до 12-битной точности. */
#define BAT_SAMPLE_TIME     ADC_SAMPLETIME_84CYCLES
/* VREFINT по даташиту требует не меньше 10 мкс выборки. */
#define VREF_SAMPLE_TIME    ADC_SAMPLETIME_144CYCLES

uint8_t Battery_Read(Battery_Data *out)
{
    uint32_t raw_bat = 0, raw_ref = 0;

    /* При отказе поля остаются такими: NAN и «неизвестно», а не нули —
     * ноль в архиве читается как «батарея пуста». */
    out->valid   = 0;
    out->volts   = NAN;
    out->vdd     = NAN;
    out->percent = BAT_PERCENT_UNKNOWN;
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

    /* Гасим внутренний опорный источник. HAL включает его при выборе канала 17
     * и снимает только когда следующим настраивают обычный канал — а следующего
     * в цикле уже нет, и блок остаётся под питанием до самого утра. */
    ADC1_COMMON->CCR &= ~ADC_CCR_TSVREFE;

    /* Деление на ноль опоры, абсурдное VDD и обрыв делителя отсекаются там же,
     * где считаются: при отказе в полях остаются NAN. */
    if (!Battery_ComputeVolts(raw_bat, raw_ref, *VREFINT_CAL_ADDR, &out->vdd, &out->volts))
        return 0;

    out->percent = Battery_VoltsToPercent(out->volts);
    out->low     = (out->volts < BAT_LOW_VOLTS) ? 1U : 0U;
    out->valid   = 1;

    return 1;
}
