#ifndef SENSOR_POLICY_H
#define SENSOR_POLICY_H

/* Перекрёстная проверка температуры воздуха по BMP280 — без HAL, чтобы
 * пересчёт и правило проверялись хостовыми тестами. Отдельный .c не заведён
 * сознательно: Debug/ не в git, и сборка CubeIDE новый файл не увидит.
 *
 * Решение владельца: BMP280 на том же модуле, что и AHT20, служит только
 * сверкой. Давление не пишется, а значение AHT20 при расхождении не
 * заменяется на NAN — строка лишь помечается в колонке air_xchk. */

#include <stdint.h>
#include <math.h>

#define BMP280_CHIP_ID   0x58U

/* Идентификаторы, у которых та же формула температуры: серийный BMP280
 * (0x58), его инженерные образцы (0x56, 0x57) и BME280 (0x60) — модули
 * «AHT20+BMP280» бывают и с ним. */
static inline uint8_t Bmp280_IdOk(uint8_t id)
{
    return (id == 0x58U || id == 0x56U || id == 0x57U || id == 0x60U) ? 1U : 0U;
}

/* Отсчёт АЦП температуры, который BMP280 отдаёт, когда измерения не было
 * (после сброса или при выключенной температуре): 0x80000. Его пересчёт дал
 * бы правдоподобное число — ровно тот класс ошибки, на котором проект уже
 * обжигался дважды. */
#define BMP280_ADC_T_SKIPPED  0x80000L

/* Окно правдоподобия — рабочий диапазон BMP280 по даташиту. */
#define BMP280_T_MIN_C  (-40.0f)
#define BMP280_T_MAX_C  (85.0f)

/* Калибровка пригодна. Нулевые или стёртые (0xFFFF) коэффициенты — это
 * не прочитавшаяся калибровка: с ними пересчёт даёт ровно 0.00 °C или мусор,
 * похожий на измерение (П2, шапка bmp280.c). У настоящего датчика dig_T1 —
 * десятки тысяч, dig_T2 — положительное. */
static inline uint8_t Bmp280_CalibOk(uint16_t t1, int16_t t2, int16_t t3)
{
    (void)t3;   /* законно бывает любым, в том числе нулём */
    if (t1 == 0U || t1 == 0xFFFFU)
        return 0;
    if (t2 <= 0)
        return 0;
    return 1;
}

/* Температура, °C, по целочисленной формуле даташита (BMP280, 3.11.3,
 * bmp280_compensate_T_int32). NAN — отсчёт «измерения не было», вне 20 бит
 * или результат вне рабочего диапазона. */
static inline float Bmp280_CompensateT(int32_t adc_t, uint16_t t1, int16_t t2, int16_t t3)
{
    if (adc_t <= 0 || adc_t >= 0xFFFFFL || adc_t == BMP280_ADC_T_SKIPPED)
        return NAN;

    /* Промежуточные — в 64 битах: в даташите они 32-битные и не переполняются
     * лишь на правдоподобных отсчётах, а мусорный отсчёт с шины дал бы
     * неопределённое поведение. На правдоподобных результат тот же. */
    int64_t var1 = ((((int64_t)adc_t >> 3) - ((int64_t)t1 << 1)) * (int64_t)t2) >> 11;
    int64_t d    = ((int64_t)adc_t >> 4) - (int64_t)t1;
    int64_t var2 = (((d * d) >> 12) * (int64_t)t3) >> 14;
    int64_t t_fine = var1 + var2;
    float t = (float)((t_fine * 5 + 128) >> 8) / 100.0f;

    if (t < BMP280_T_MIN_C || t > BMP280_T_MAX_C)
        return NAN;
    return t;
}

/* Колонка air_xchk. */
#define SENSOR_XCHK_OK       0U   /* AHT20 и BMP280 сошлись */
#define SENSOR_XCHK_DIFF     1U   /* разошлись больше порога — AHT20 под подозрением */
#define SENSOR_XCHK_NO_BMP   2U   /* BMP280 не ответил — сверки нет */
#define SENSOR_XCHK_NO_AHT   3U   /* у AHT20 нет значения — сверять нечего */

/* Порог расхождения. Паспортная точность: AHT20 ±0.3 °C (типовая), BMP280
 * ±1.0 °C (0..65 °C) — в худшем случае они расходятся на 1.3 °C, будучи
 * исправными. Ещё ~0.5 °C — на разный самонагрев и тепловую инерцию двух
 * корпусов на одной плате модуля сразу после подачи питания. 2.0 °C —
 * сверх этого исправные датчики не расходятся; больше — повод не верить. */
#define SENSOR_XCHK_MAX_DIFF_C  2.0f

static inline uint8_t Sensor_AirCrossCheck(float aht_t, float bmp_t)
{
    if (isnan(aht_t))
        return SENSOR_XCHK_NO_AHT;
    if (isnan(bmp_t))
        return SENSOR_XCHK_NO_BMP;
    return (fabsf(aht_t - bmp_t) > SENSOR_XCHK_MAX_DIFF_C) ? SENSOR_XCHK_DIFF : SENSOR_XCHK_OK;
}

#endif
