#ifndef STORAGE_POLICY_H
#define STORAGE_POLICY_H

/* Решения о том, когда трогать архив на флеш. Правила FAT-тома (формат,
 * MBR, переименование, место) ушли вместе с ним в П5а: архив — кольцевой
 * журнал (log_ring.h). Без HAL и без регистров —
 * чтобы их можно было проверить хостовыми тестами. Отдельный .c не заведён
 * сознательно: Debug/ не в git, и сборка CubeIDE новый файл не увидит. */

#include <stdint.h>
#include <math.h>
#include <string.h>

/* Ниже этого VDD стирание и программирование SST26VF016B идут вне паспорта
 * (2.7–3.6 В), а пик тока стирания на исходе банки может уронить питание
 * посреди стирания сектора журнала. Запас 0.2 В над паспортом — на просадку
 * под нагрузкой, которую VREFINT в момент замера не видит. */
#define STORAGE_MIN_VDD_VOLTS  2.9f

/* Метка времени последней строки архива — для Policy_TimeTrusted (A6).
 * tail — последние n байт файла; from_start — tail начинается с начала
 * файла (иначе первая строка в нём, скорее всего, обрезана и не в счёт).
 * Ищется последняя строка, которая начинается с "YYYY-MM-DD hh:mm:ss";
 * заголовок и обрывки не подходят. Метка (19 символов и ноль) — в out[20].
 * Возвращает 1, если нашлась. */
#define STORAGE_TS_LEN  19U

static inline uint8_t Storage_IsTimestamp(const char *p)
{
    static const char pat[] = "dddd-dd-dd dd:dd:dd";
    for (uint32_t i = 0; i < STORAGE_TS_LEN; i++)
    {
        if (pat[i] == 'd' ? (p[i] < '0' || p[i] > '9') : (p[i] != pat[i]))
            return 0;
    }
    return 1;
}

static inline uint8_t Storage_LastTimestamp(const char *tail, uint32_t n, uint8_t from_start,
                                            char *out)
{
    /* С конца: начало строки — сразу после '\n' (или начало файла). */
    for (uint32_t i = n; i > 0U; i--)
    {
        uint32_t start = i;   /* кандидат: tail[i] — сразу после tail[i - 1] */
        if (tail[i - 1U] != '\n')
            continue;
        if (n - start >= STORAGE_TS_LEN && Storage_IsTimestamp(tail + start))
        {
            memcpy(out, tail + start, STORAGE_TS_LEN);
            out[STORAGE_TS_LEN] = '\0';
            return 1;
        }
    }
    if (from_start && n >= STORAGE_TS_LEN && Storage_IsTimestamp(tail))
    {
        memcpy(out, tail, STORAGE_TS_LEN);
        out[STORAGE_TS_LEN] = '\0';
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* Можно ли MSC (из прерывания OTG) читать флеш прямо сейчас. Не готова —
 * значит не инициализирована или спит, и будить её из прерывания нельзя:
 * задержки там не идут. Занята — основной поток посреди операции журнала, и
 * чтение оборвало бы его кадр SPI или отдало хосту полузаписанный журнал.
 * В обоих случаях хост получает «не готов» и повторяет запрос сам. */
static inline uint8_t Storage_MscMayAccess(uint8_t flash_ready, uint8_t flash_busy)
{
    return (flash_ready && !flash_busy) ? 1U : 0U;
}

/* Можно ли сейчас писать на флеш при таком питании. Журнал переживает обрыв
 * посреди записи (G2), но программирование вне паспорта по VDD может «пройти»
 * со слабо записанными битами, которые поплывут потом, а пик тока стирания
 * на исходе банки сам роняет питание. Пропущенная строка стоит одного часа
 * данных.
 * Низкое VDD запрещает запись всегда, даже при valid == 0: Battery_Read
 * отдаёт VDD по VREFINT и тогда, когда банка вне шкалы (глубокий разряд,
 * LDO в дропауте) — это и есть самый опасный для флеш случай (R2).
 * Если неизвестно и VDD (vdd — NAN: АЦП не ответил, опора 0), запись не
 * запрещается: иначе отказ АЦП навсегда остановил бы архив, а о питании мы
 * тогда просто ничего не знаем. */
static inline uint8_t Storage_MayWrite(uint8_t bat_valid, uint8_t bat_low, float vdd)
{
    if (!isnan(vdd) && vdd < STORAGE_MIN_VDD_VOLTS)
        return 0;
    if (bat_valid && bat_low)
        return 0;
    return 1;
}

/* Замер не записался — оставить ли его в очереди до следующего цикла (T6).
 * Да, если журнал не был открыт (на флеш ничего не ушло) или после отказа
 * он всё ещё открыт (слот не записан или испорчен — номер пропущен, дубля
 * не будет). Нет, если отказ закрыл журнал: не прочиталась сверка, и запись,
 * возможно, уже лежит на флеш — повтор дал бы дубль строки. */
static inline uint8_t Storage_RequeueFailed(uint8_t was_open, uint8_t still_mounted)
{
    return (!was_open || still_mounted) ? 1U : 0U;
}

#endif
