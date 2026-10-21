#ifndef BATTERY_H
#define BATTERY_H

#include "adc.h"
#include "battery_curve.h"   /* BAT_DIVIDER_RATIO и пересчёт — там, без HAL */

/* Ниже этого напряжения XC6206 выходит из режима стабилизации: VDD начинает
 * следовать за банкой, и опорное напряжение АЦП уплывает вместе с ним.
 * Показания влажности почвы с этого момента считаем недостоверными. */
#define BAT_LOW_VOLTS       3.30f

typedef struct {
    float   volts;     /* напряжение банки, NAN при отказе */
    float   vdd;       /* реальное VDD, вычисленное через VREFINT, NAN при отказе */
    uint8_t percent;   /* заряд, % по кривой разряда Li-ion; BAT_PERCENT_UNKNOWN при отказе */
    uint8_t low;       /* 1 — ниже BAT_LOW_VOLTS */
    uint8_t valid;     /* 0 — измерить не удалось */
} Battery_Data;

/* Измеряет банку и вычисляет реальное VDD по внутреннему опорному источнику.
 * Возвращает 1 при успехе. */
uint8_t Battery_Read(Battery_Data *out);

#endif
