#ifndef VFAT_H
#define VFAT_H

/* Виртуальный FAT-том для USB поверх журнала (П5б).
 *
 * Single-header: объявления здесь, реализация — под VFAT_IMPLEMENTATION;
 * её включает ровно один .c прошивки (usbd_storage_if.c) и хостовые тесты.
 * Флеш модуль трогает только через LogRing_Read: том собирается из журнала
 * на лету, писать ему нечем и некуда (G3).
 *
 * Что видит хост: диск 16 МБ, MBR, раздел FAT16 «FIELDSENSOR» только для
 * чтения, в корне — data.txt (заголовок CSV и строки замеров от старой к
 * новой, перевод строки \r\n, как писал старый f_puts), data_old.txt (текст
 * старого архива, пока он есть в журнале) и короткий info.txt.
 *
 * Геометрия (расчёт — в отчёте П5б):
 *   - FAT16, а не FAT12: Android FAT12 не монтирует (история проекта).
 *     Тип раздела 0x0E (FAT16 LBA) и раздел с LBA 63 — как у тома старых
 *     прошивок, который телефон заказчика принимал.
 *   - Самый длинный data.txt — весь журнал замерами: 32 768 слотов по
 *     строке до ~190 Б при любых значениях датчиков ≈ 6.2 МБ; даже при
 *     бессмысленных float (1e38 во всех полях, строка ~375 Б) — 12.3 МБ.
 *     data_old.txt берёт те же слоты, так что вместе с ним — не больше.
 *     Кластер 2 КБ на 16 МБ даёт 8152 кластера (16.7 МБ данных): в FAT16
 *     с запасом вдвое от границы FAT12 (4085) и далеко от верхней (65 525).
 *   - Файлы непрерывны, цепочки FAT вычисляются арифметикой.
 *
 * Смещение в файле -> запись. Длина строки CSV заранее неизвестна, поэтому
 * при подключении USB журнал проходится один раз (VFat_Build): считаются
 * размеры файлов и запоминается смещение начала каждого сектора журнала
 * (64 записи) в data.txt и в data_old.txt — 2 × 513 × 4 Б ≈ 4 КБ RAM.
 * Чтение сектора файла ищет ближайшую точку индекса двоичным поиском и
 * форматирует вперёд, а курсор продолжения делает последовательное чтение
 * (так копирует и Windows, и Android) почти бесплатным: по 3–4 строки на
 * сектор вместо до 67 в худшем случае. Пока идёт сессия, журнал неподвижен
 * (A3): main.c ставит замеры в очередь в RAM, так что размеры и индекс
 * верны до отключения кабеля.
 *
 * Строка замера печатается своим форматтером (VFat_FormatMeas), а не
 * snprintf: newlib-nano тратит на строку ~37 тыс. инструкций (~3 мс на
 * 16 МГц), и проход журнала при подключении занимал бы ~100 с. Свой даёт
 * те же байты — значение float печатается точно, с округлением половины к
 * чётному, как у printf newlib и glibc (сверено тестом с LogRecord_FormatCsv
 * на хосте и под qemu с newlib-nano). Что он печатать не берётся (|x| ≥ 2^20,
 * то есть больше миллиона), уходит в LogRecord_FormatCsv. А чтобы смена libc в
 * сборке не испортила файл молча, VFat_Build сверяет оба форматтера на
 * выборке записей и при расхождении переходит на LogRecord_FormatCsv целиком
 * (медленно, но верно; отмечается в info.txt). */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "log_ring.h"

#define VFAT_SECTOR        512U
#define VFAT_DISK_SECTORS  32768U   /* 16 МБ — то, что заявляется хосту */
#define VFAT_PART_LBA      63U      /* загрузочный сектор раздела */
#define VFAT_PART_SECTORS  (VFAT_DISK_SECTORS - VFAT_PART_LBA)
#define VFAT_SPC           4U       /* секторов в кластере: 2 КБ */
#define VFAT_CLUSTER       (VFAT_SPC * VFAT_SECTOR)
#define VFAT_RSVD          1U
#define VFAT_NFATS         2U
#define VFAT_FAT_SECTORS   32U
#define VFAT_ROOT_ENTS     512U
#define VFAT_ROOT_SECTORS  (VFAT_ROOT_ENTS * 32U / VFAT_SECTOR)
/* Секторы — от начала раздела. */
#define VFAT_FAT1          VFAT_RSVD
#define VFAT_ROOT          (VFAT_RSVD + VFAT_NFATS * VFAT_FAT_SECTORS)
#define VFAT_DATA          (VFAT_ROOT + VFAT_ROOT_SECTORS)
#define VFAT_CLUSTERS      ((VFAT_PART_SECTORS - VFAT_DATA) / VFAT_SPC)   /* 8152 */

/* Граница FAT16 по спецификации Microsoft (её же держат Linux и FatFs):
 * 4085 ≤ N < 65525. Таблица должна вмещать N + 2 записи по 2 байта. */
typedef char VFat_AssertFat16[(VFAT_CLUSTERS >= 4085U && VFAT_CLUSTERS < 65525U) ? 1 : -1];
typedef char VFat_AssertFatSize[((VFAT_CLUSTERS + 2U) * 2U <= VFAT_FAT_SECTORS * VFAT_SECTOR) ? 1 : -1];

/* Точка индекса — на каждый сектор журнала, плюс конец. */
#define VFAT_CHUNKS_MAX    LOG_FLASH_SECTORS
/* Буфер строки. LOG_CSV_LINE_MAX (224) хватает всему, что пишет прошивка,
 * но строка с float вроде 1e38 длиннее, и LogRecord_FormatCsv с таким
 * пределом вернул бы -1 — замер молча выпал бы из файла (замечание отчёта
 * П5а). Худшая строка — пять полей "%.2f" и одно "%.3f" по ~44 знака, сырые
 * отсчёты до 65534, — ~375 Б с \r\n; 448 — с запасом. */
#define VFAT_LINE_BUF      448U
/* Худший data.txt: весь журнал — строки по VFAT_LINE_BUF (+ заголовок и info.txt). */
typedef char VFat_AssertRoom[((LOG_FLASH_SECTORS * LOG_SLOTS * VFAT_LINE_BUF) / VFAT_CLUSTER + 4U
                              <= VFAT_CLUSTERS) ? 1 : -1];

#define VFAT_HEADER        LOG_CSV_HEADER "\r\n"
#define VFAT_HEADER_LEN    (sizeof(VFAT_HEADER) - 1U)

/* Состояние тома. */
#define VFAT_OK         0U
#define VFAT_NO_ARCHIVE 1U   /* журнал не открыт — только info.txt с причиной */
#define VFAT_EIO        2U   /* флеш не прочиталась при сборке — то же */

#define VFAT_FILE_DATA  0U
#define VFAT_FILE_OLD   1U
#define VFAT_FILE_INFO  2U
#define VFAT_FILES      3U

typedef struct {
    uint32_t i;     /* запись журнала, с которой продолжать */
    uint32_t pos;   /* смещение её строки в файле */
    uint8_t  bads;  /* испорченных слотов от начала её сектора журнала до неё */
    uint8_t  valid;
} VFatCursor;

typedef struct {
    const LogRing *ring;
    const char    *fw;              /* строка версии прошивки для info.txt */
    uint8_t  state;
    uint8_t  slow;                  /* форматтеры разошлись — всё через LogRecord_FormatCsv */
    uint32_t n;                     /* записей журнала при сборке */
    uint32_t nchunks;
    uint32_t size[VFAT_FILES];
    uint16_t clus[VFAT_FILES];      /* первый кластер (0 — файла нет или он пуст) */
    uint16_t ncl[VFAT_FILES];
    uint8_t  present[VFAT_FILES];
    uint16_t fdate, ftime;
    uint32_t serial;
    /* Для info.txt. */
    uint32_t n_meas, n_bad, n_skip, n_text, n_import;
    uint32_t imp_files, imp_dropped;
    char     first_ts[20], last_ts[20];
    uint8_t  last_time_ok;
    uint32_t last_wf, last_lps, last_boot;
    /* Индекс: смещение в файле начала сектора журнала k (записи k*64). */
    uint32_t off[2][VFAT_CHUNKS_MAX + 1U];
    /* Испорченных слотов в секторе журнала при сборке. Чтение сверяет с ним
     * каждый встреченный испорченный слот: разовый сбой чтения посреди
     * сектора журнала иначе сдвинул бы все следующие байты файла, а
     * поймался бы только на его границе. */
    uint8_t  nbad[VFAT_CHUNKS_MAX + 1U];
    VFatCursor cur[2];
} VFat;

/* Собрать том по журналу (r == NULL или не открыт — том с одним info.txt).
 * Вызывается в основном потоке до подъёма USB: проходит весь журнал (до
 * 32 тыс. чтений по 64 Б), пинает сторожа через r->ops->kick. Возвращает
 * VFAT_OK или причину. */
uint8_t VFat_Build(VFat *v, const LogRing *r, const char *fw);

/* Сектор диска lba (0..VFAT_DISK_SECTORS-1) в buf (512 Б). 1 — готово, 0 —
 * отказ флеш или журнал разошёлся с индексом. Отказ, а не сдвинутые байты:
 * хост получит ошибку чтения (04/11). Windows её повторит, Linux/Android —
 * нет, копирование оборвётся с ошибкой ввода-вывода. */
uint8_t VFat_Read(VFat *v, uint32_t lba, uint8_t *buf);

/* Строка data.txt для записи замера: те же байты, что LogRecord_FormatCsv
 * (с пределом VFAT_LINE_BUF - 1), но с \r\n. out — не меньше VFAT_LINE_BUF.
 * Возвращает длину или -1 (не замер — в data.txt не попадает). */
int VFat_FormatMeas(const LogRecord *r, char *out);

/* Нужна ли для сектора lba флеш (данные data.txt / data_old.txt). Служебные
 * секторы и info.txt собираются из RAM — их хост прочтёт и при флеш, не
 * готовой к чтению. */
uint8_t VFat_UsesFlash(const VFat *v, uint32_t lba);
int VFat_FormatMeasSlow(const LogRecord *r, char *out);

/* ====================================================== реализация */
#ifdef VFAT_IMPLEMENTATION

static void VFat_Put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void VFat_Put32(uint8_t *p, uint32_t v) { VFat_Put16(p, v); VFat_Put16(p + 2, v >> 16); }

/* ------------------------------------------------------ строка замера */

static char *VFat_PutU(char *o, uint32_t v)
{
    char t[10];
    uint32_t k = 0;
    do { t[k++] = (char)('0' + v % 10U); v /= 10U; } while (v);
    while (k) *o++ = t[--k];
    return o;
}

static char *VFat_PutD(char *o, int v)
{
    if (v < 0) { *o++ = '-'; return VFat_PutU(o, 0U - (uint32_t)v); }
    return VFat_PutU(o, (uint32_t)v);
}

/* "%.Nf" от (double)f, N = 0, 2, 3. Значение float точно равно man × 2^e;
 * man × 10^N < 2^34, так что его можно округлить до целого точно, со
 * сдвигом и остатком: половина — к чётному, как в printf (newlib и glibc
 * печатают точное двоичное значение с правильным округлением; проверено
 * тестом на -0, -nan, inf и половинках). Знак — по знаковому биту:
 * "-0.00", "-nan". |f| ≥ 2^20 (больше миллиона — датчики такого не дают)
 * не печатается (NULL) — это дело snprintf: тогда целая часть с дробью
 * всегда умещается в uint32. */
static char *VFat_PutF(char *o, float f, uint32_t prec)
{
    static const uint32_t P10[4] = { 1U, 10U, 100U, 1000U };
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t ex = (bits >> 23) & 0xFFU, man = bits & 0x7FFFFFU;
    if (ex >= 127U + 20U && ex != 0xFFU)
        return NULL;
    if (bits >> 31)
        *o++ = '-';
    if (ex == 0xFFU)
    {
        memcpy(o, man ? "nan" : "inf", 3);
        return o + 3;
    }
    uint32_t s = (ex == 0U) ? 149U : 150U - ex;   /* f = man × 2^-s */
    if (ex != 0U)
        man |= 0x800000U;
    uint64_t n = (uint64_t)man * P10[prec];
    uint32_t q = 0;
    /* s ≥ 40: n / 2^s < 2^34 / 2^40 — меньше половины, округляется в 0.
     * Иначе s ≥ 4, и n >> s < 2^30. */
    if (s < 40U)
    {
        uint64_t rem  = n & ((1ULL << s) - 1U);
        uint64_t half = 1ULL << (s - 1U);
        q = (uint32_t)(n >> s);
        if (rem > half || (rem == half && (q & 1U)))
            q++;
    }
    char t[12];
    uint32_t k = 0;
    do { t[k++] = (char)('0' + q % 10U); q /= 10U; } while (q || k <= prec);
    while (k > prec) *o++ = t[--k];
    if (prec)
    {
        *o++ = '.';
        while (k) *o++ = t[--k];
    }
    return o;
}

int VFat_FormatMeasSlow(const LogRecord *r, char *out)
{
    int n = LogRecord_FormatCsv(r, out, VFAT_LINE_BUF - 1U);
    if (n <= 0)
        return -1;
    out[n - 1] = '\r';
    out[n] = '\n';
    return n + 1;
}

/* Поля — в том же порядке и с теми же выражениями, что snprintf в
 * LogRecord_FormatMeas. Изменится формат там — тест сверки с ним упадёт. */
int VFat_FormatMeas(const LogRecord *r, char *out)
{
    if (LogRecord_Type(r) != LOG_TYPE_MEAS)
        return -1;
    /* Метку UnpackMeas печатает через snprintf — это 2/3 цены строки.
     * Распаковывается копия с «неразобранной» меткой (тогда snprintf не
     * зовётся), а метка печатается здесь по тому же правилу: 0xFF в годе
     * или поле больше 99 — LOG_TS_UNKNOWN. */
    const uint8_t *ts = r->b + LOG_PAYLOAD_OFS;
    LogRecord c = *r;
    c.b[LOG_PAYLOAD_OFS] = 0xFF;
    LogMeas m;
    LogRecord_UnpackMeas(&c, &m);

    char *o = out;
    uint8_t bad = (ts[0] == 0xFFU);
    for (uint32_t i = 1; i < 6U; i++)
        if (ts[i] > 99U)
            bad = 1;
    if (bad)
    {
        memcpy(o, LOG_TS_UNKNOWN, sizeof(LOG_TS_UNKNOWN) - 1U);
        o += sizeof(LOG_TS_UNKNOWN) - 1U;
    }
    else
    {
        static const char SEP[6] = { '-', '-', ' ', ':', ':', 0 };
        uint32_t y = 2000U + ts[0];
        *o++ = (char)('0' + y / 1000U); *o++ = (char)('0' + y / 100U % 10U);
        *o++ = (char)('0' + y / 10U % 10U); *o++ = (char)('0' + y % 10U);
        for (uint32_t i = 1; i < 6U; i++)
        {
            *o++ = SEP[i - 1U];
            *o++ = (char)('0' + ts[i] / 10U);
            *o++ = (char)('0' + ts[i] % 10U);
        }
    }
#define VFAT_F(x, p) do { *o++ = ','; o = VFat_PutF(o, (x), (p)); if (!o) return VFat_FormatMeasSlow(r, out); } while (0)
#define VFAT_U(x)    do { *o++ = ','; o = VFat_PutU(o, (uint32_t)(x)); } while (0)
#define VFAT_D(x)    do { *o++ = ','; o = VFat_PutD(o, (x)); } while (0)
    VFAT_F(m.air_temp, 2);
    VFAT_F(m.air_hum, 2);
    VFAT_F(m.soil_temp, 2);
    VFAT_F(m.soil_hum, 2);
    VFAT_F(m.bat_valid ? (float)m.bat_percent : NAN, 0);
    VFAT_F(m.bat_valid ? m.bat_volts : NAN, 2);
    VFAT_F(m.vdd, 3);
    VFAT_U(m.bat_valid && !m.bat_low);
    VFAT_U(m.lse_ok);
    VFAT_U(m.time_ok);
    VFAT_D(m.aht_init_st);
    VFAT_D(m.aht_read_st);
    VFAT_U(m.ds_err);
    VFAT_U(m.air_xchk);
    VFAT_U(m.i2c_rec);
    VFAT_U(m.sens_off);
    VFAT_U(m.write_fails);
    VFAT_U(m.lowpower_skips);
    VFAT_U(m.reset_cause);
    VFAT_U(m.boot_count);
    VFAT_F(m.soil_raw, 0);
    VFAT_F(m.bat_raw, 0);
#undef VFAT_F
#undef VFAT_U
#undef VFAT_D
    *o++ = '\r';
    *o++ = '\n';
    /* Каждое число здесь короче 13 знаков, строка — не длиннее ~190:
     * предел буфера (тогда LogRecord_FormatCsv дал бы -1) недостижим, но
     * проверяется тем же правилом. */
    int n = (int)(o - out);
    return ((uint32_t)(n - 1) >= VFAT_LINE_BUF - 1U) ? -1 : n;
}

/* Эталон самопроверки при сборке. Хостовый тест подменяет его, чтобы
 * проверить переход на медленный путь; в прошивке — LogRecord_FormatCsv. */
#ifndef VFAT_CHECK_LINE
#define VFAT_CHECK_LINE VFat_FormatMeasSlow
#endif

/* Строка записи для файла which; 0 — запись не из этого файла. */
static int VFat_Line(const VFat *v, uint32_t which, const LogRecord *rec, char *line,
                     const uint8_t **text)
{
    if (which == VFAT_FILE_OLD)
        return (int)LogRecord_Text(rec, text, NULL);
    int n = v->slow ? VFat_FormatMeasSlow(rec, line) : VFat_FormatMeas(rec, line);
    *text = (const uint8_t *)line;
    return (n < 0) ? 0 : n;
}

/* ----------------------------------------------------------- сборка */

static uint16_t VFat_FatDate(uint32_t y, uint32_t mo, uint32_t d)
{
    return (uint16_t)(((y - 1980U) << 9) | (mo << 5) | d);
}

/* Дата файлов — по последнему замеру, если часам можно верить. Иначе (и при
 * разобранной не до конца метке) — фиксированная 2026-01-01 00:00. */
static void VFat_Stamp(VFat *v)
{
    v->fdate = VFat_FatDate(2026U, 1U, 1U);
    v->ftime = 0;
    if (!v->last_time_ok || v->last_ts[0] == '\0')
        return;
    const char *t = v->last_ts;
    uint32_t y = LogRecord_Digits(t, 4), mo = LogRecord_Digits(t + 5, 2), d = LogRecord_Digits(t + 8, 2);
    uint32_t h = LogRecord_Digits(t + 11, 2), mi = LogRecord_Digits(t + 14, 2), s = LogRecord_Digits(t + 17, 2);
    if (y < 1980U || y > 2107U || mo < 1U || mo > 12U || d < 1U || d > 31U ||
        h > 23U || mi > 59U || s > 59U)
        return;
    v->fdate = VFat_FatDate(y, mo, d);
    v->ftime = (uint16_t)((h << 11) | (mi << 5) | (s / 2U));
}

static uint32_t VFat_RenderInfo(const VFat *v, char *out, uint32_t size)
{
    int n;
    const char *fw = v->fw ? v->fw : "?";
    if (v->state != VFAT_OK)
        n = snprintf(out, size,
                     "FieldSensor %s\r\n"
                     "ARCHIVE NOT AVAILABLE: %s.\r\n"
                     "Unplug the cable, wait a minute and plug it in again.\r\n",
                     fw, (v->state == VFAT_EIO) ? "flash read error"
                         : "log is not open (flash error, or the old archive waits for battery power to be imported)");
    else
        n = snprintf(out, size,
                     "FieldSensor %s\r\n"
                     "data.txt: %lu lines, first %s, last %s%s\r\n"
                     "damaged log slots skipped: %lu\r\n"
                     "last write_fails=%lu lowpower_skips=%lu boot_count=%lu\r\n"
                     "%s%s%s%s%s",
                     fw, (unsigned long)v->n_meas,
                     v->first_ts[0] ? v->first_ts : "-", v->last_ts[0] ? v->last_ts : "-",
                     (v->last_ts[0] && !v->last_time_ok) ? " (clock not trusted)" : "",
                     (unsigned long)(v->n_bad + v->n_skip),
                     (unsigned long)v->last_wf, (unsigned long)v->last_lps, (unsigned long)v->last_boot,
                     v->present[VFAT_FILE_OLD] ? "data_old.txt: text of the old archive" : "",
                     (v->n_import && v->imp_dropped) ? (v->present[VFAT_FILE_OLD]
                         ? ", some old files did not fit" : "old archive did not fit") : "",
                     (v->present[VFAT_FILE_OLD] || (v->n_import && v->imp_dropped)) ? "\r\n" : "",
                     v->slow ? "slow line formatter (self-check failed)" : "",
                     v->slow ? "\r\n" : "");
    if (n < 0)
        return 0;
    return ((uint32_t)n >= size) ? size - 1U : (uint32_t)n;
}

static void VFat_Layout(VFat *v)
{
    char info[VFAT_SECTOR];
    v->size[VFAT_FILE_INFO] = VFat_RenderInfo(v, info, sizeof(info));
    v->present[VFAT_FILE_INFO] = 1;
    uint32_t c = 2;
    for (uint32_t f = 0; f < VFAT_FILES; f++)
    {
        v->ncl[f] = (uint16_t)((v->size[f] + VFAT_CLUSTER - 1U) / VFAT_CLUSTER);
        v->clus[f] = (uint16_t)(v->ncl[f] ? c : 0U);
        c += v->ncl[f];
    }
    VFat_Stamp(v);
}

/* Запись журнала с повтором: разовое искажённое или сорванное чтение
 * (помеха на SPI) не должно ни выкинуть строку из индекса, ни сдвинуть
 * файл при чтении. Настоящая порча слота стабильна — повтор её не скроет. */
static uint8_t VFat_ReadRec(const LogRing *r, uint32_t i, LogRecord *rec)
{
    uint8_t st = LOG_READ_EIO;
    for (uint32_t k = 0; k < 3U; k++)
    {
        st = LogRing_Read(r, i, rec);
        if (st == LOG_READ_OK || st == LOG_READ_NONE)
            break;
    }
    return st;
}

static void VFat_Kick(const LogRing *r)
{
    if (r->ops && r->ops->kick)
        r->ops->kick();
}

/* Проход журнала: размеры файлов, индекс, сведения для info.txt.
 * check — сверять быстрый форматтер с LogRecord_FormatCsv на выборке. */
static uint8_t VFat_Scan(VFat *v, uint8_t check)
{
    const LogRing *r = v->ring;
    uint32_t pos[2] = { VFAT_HEADER_LEN, 0U };
    LogRecord rec, last;
    char line[VFAT_LINE_BUF], ref[VFAT_LINE_BUF];

    for (uint32_t i = 0; i < v->n; i++)
    {
        if (i % LOG_SLOTS == 0U)
        {
            v->off[0][i / LOG_SLOTS] = pos[0];
            v->off[1][i / LOG_SLOTS] = pos[1];
            VFat_Kick(r);
        }
        uint8_t st = VFat_ReadRec(r, i, &rec);
        if (st == LOG_READ_EIO || st == LOG_READ_NONE)
            return VFAT_EIO;
        if (st == LOG_READ_BAD)
        {
            v->n_bad++;
            v->nbad[i / LOG_SLOTS]++;
            continue;
        }
        uint8_t t = LogRecord_Type(&rec);
        if (t == LOG_TYPE_MEAS)
        {
            int n = v->slow ? VFat_FormatMeasSlow(&rec, line) : VFat_FormatMeas(&rec, line);
            /* Выборка — первый замер и каждый 512-й слот: ~64 вызова
             * snprintf, ~0.2 с на плате. */
            if (check && (v->n_meas == 0U || i % 512U == 0U))
            {
                int m = VFAT_CHECK_LINE(&rec, ref);
                if (m != n || (n > 0 && memcmp(line, ref, (size_t)n) != 0))
                    return 0xFFU;
            }
            if (n < 0)
            {
                v->n_skip++;
                continue;
            }
            pos[0] += (uint32_t)n;
            /* Распаковка — только первой и последней: она дорогая. */
            if (v->n_meas++ == 0U)
            {
                LogMeas m;
                LogRecord_UnpackMeas(&rec, &m);
                memcpy(v->first_ts, m.ts, sizeof(v->first_ts));
            }
            last = rec;
        }
        else
        {
            v->n_import++;
            if (t == LOG_TYPE_TEXT)
            {
                pos[1] += LogRecord_Text(&rec, NULL, NULL);
                v->n_text++;
            }
            else if (t == LOG_TYPE_IMPORT_END)
            {
                v->imp_files = rec.b[LOG_PAYLOAD_OFS];
                v->imp_dropped = rec.b[LOG_PAYLOAD_OFS + 5];
            }
        }
    }
    if (v->n_meas)
    {
        LogMeas m;
        LogRecord_UnpackMeas(&last, &m);
        memcpy(v->last_ts, m.ts, sizeof(v->last_ts));
        v->last_time_ok = m.time_ok;
        v->last_wf = m.write_fails;
        v->last_lps = m.lowpower_skips;
        v->last_boot = m.boot_count;
    }
    v->off[0][v->nchunks] = pos[0];
    v->off[1][v->nchunks] = pos[1];
    v->size[VFAT_FILE_DATA] = pos[0];
    v->size[VFAT_FILE_OLD] = pos[1];
    return VFAT_OK;
}

uint8_t VFat_Build(VFat *v, const LogRing *r, const char *fw)
{
    memset(v, 0, sizeof(*v));
    v->ring = r;
    v->fw = fw;
    if (r == NULL || !r->mounted)
    {
        v->state = VFAT_NO_ARCHIVE;
        VFat_Layout(v);
        return v->state;
    }
    v->n = LogRing_Count(r);
    v->nchunks = (v->n + LOG_SLOTS - 1U) / LOG_SLOTS;
    if (v->nchunks > VFAT_CHUNKS_MAX)
        v->state = VFAT_EIO;
    else
    {
        uint8_t st = VFat_Scan(v, 1);
        if (st == 0xFFU)
        {
            /* Быстрый форматтер разошёлся с LogRecord_FormatCsv: libc сборки
             * печатает иначе. Файл важнее скорости. */
            const LogRing *keep = r;
            memset(v, 0, sizeof(*v));
            v->ring = keep;
            v->fw = fw;
            v->n = LogRing_Count(r);
            v->nchunks = (v->n + LOG_SLOTS - 1U) / LOG_SLOTS;
            v->slow = 1;
            st = VFat_Scan(v, 0);
        }
        v->state = st;
    }
    if (v->state != VFAT_OK)
    {
        /* Сломанный data.txt хуже никакого: заказчик унёс бы обрывок за
         * архив. Остаётся только info.txt с причиной. */
        v->size[VFAT_FILE_DATA] = v->size[VFAT_FILE_OLD] = 0;
    }
    else
    {
        v->present[VFAT_FILE_DATA] = 1;
        v->present[VFAT_FILE_OLD] = (v->size[VFAT_FILE_OLD] > 0U);
    }
    v->serial = 0x46530000UL ^ (LogRing_FirstSeq(r) * 2654435761UL) ^ v->n;
    VFat_Layout(v);
    return v->state;
}

/* ----------------------------------------------------------- чтение */

/* Сектор файла: байты [x, x+len) файла which (data или old). */
static uint8_t VFat_FileRead(VFat *v, uint32_t which, uint32_t x, uint8_t *buf, uint32_t len)
{
    const uint32_t *off = v->off[which];
    uint32_t hdr = (which == VFAT_FILE_DATA) ? VFAT_HEADER_LEN : 0U;
    uint32_t end = x + len;

    if (x < hdr)
    {
        uint32_t k = ((end < hdr) ? end : hdr) - x;
        memcpy(buf, VFAT_HEADER + x, k);
    }
    if (end <= hdr)
        return 1;
    uint32_t y = (x > hdr) ? x : hdr;

    /* Последний сектор журнала, начавшийся не позже y. */
    uint32_t lo = 0, hi = v->nchunks;   /* off[lo] ≤ y < off[hi] */
    while (hi - lo > 1U)
    {
        uint32_t mid = (lo + hi) / 2U;
        if (off[mid] <= y) lo = mid; else hi = mid;
    }
    uint32_t i = lo * LOG_SLOTS, p = off[lo], bads = 0;
    VFatCursor *c = &v->cur[which];
    if (c->valid && c->pos <= y && c->i >= i)
    {
        i = c->i;
        p = c->pos;
        bads = c->bads;
    }

    LogRecord rec;
    char line[VFAT_LINE_BUF];
    for (;;)
    {
        if (p >= end)
            break;
        /* Индекс и журнал должны сходиться на каждой границе сектора:
         * если запись прочиталась иначе, чем при сборке, отдать хосту
         * сдвинутые байты хуже, чем ошибку чтения. */
        if (i >= v->n || (i % LOG_SLOTS == 0U && p != off[i / LOG_SLOTS]))
        {
            c->valid = 0;
            return 0;
        }
        if (i % LOG_SLOTS == 0U)
            bads = 0;
        uint8_t st = VFat_ReadRec(v->ring, i, &rec);
        if (st == LOG_READ_BAD && ++bads > v->nbad[i / LOG_SLOTS])
            st = LOG_READ_EIO;   /* при сборке слот читался — сбой сейчас */
        if (st == LOG_READ_EIO || st == LOG_READ_NONE)
        {
            c->valid = 0;
            return 0;
        }
        if (st == LOG_READ_OK)
        {
            const uint8_t *t;
            uint32_t n = (uint32_t)VFat_Line(v, which, &rec, line, &t);
            if (n)
            {
                if (p + n > y)
                {
                    uint32_t a = (p > y) ? p : y;
                    uint32_t b = (p + n < end) ? p + n : end;
                    memcpy(buf + (a - x), t + (a - p), b - a);
                }
                if (p + n > end)
                {
                    /* Строка переходит в следующий сектор — с неё и начнём. */
                    c->i = i; c->pos = p; c->bads = (uint8_t)bads; c->valid = 1;
                    return 1;
                }
                p += n;
            }
        }
        i++;
    }
    c->i = i; c->pos = p; c->bads = (uint8_t)bads; c->valid = 1;
    return 1;
}

uint8_t VFat_UsesFlash(const VFat *v, uint32_t lba)
{
    if (lba < VFAT_PART_LBA + VFAT_DATA || lba >= VFAT_DISK_SECTORS)
        return 0;
    uint32_t c = (lba - VFAT_PART_LBA - VFAT_DATA) / VFAT_SPC + 2U;
    for (uint32_t f = VFAT_FILE_DATA; f <= VFAT_FILE_OLD; f++)
        if (v->ncl[f] && c >= v->clus[f] && c < (uint32_t)v->clus[f] + v->ncl[f])
            return 1;
    return 0;
}

static void VFat_Mbr(uint8_t *b)
{
    uint8_t *pe = b + 446;
    /* CHS при 255 головах и 63 секторах на дорожку; хосты берут LBA. */
    uint32_t last = VFAT_DISK_SECTORS - 1U;
    uint32_t cyl = last / (255U * 63U), head = (last / 63U) % 255U, sect = last % 63U + 1U;
    pe[0] = 0x00;
    pe[1] = 1; pe[2] = 1; pe[3] = 0;               /* LBA 63 = C0 H1 S1 */
    pe[4] = 0x0E;                                   /* FAT16 LBA — Android */
    pe[5] = (uint8_t)head;
    pe[6] = (uint8_t)(sect | ((cyl >> 2) & 0xC0U));
    pe[7] = (uint8_t)cyl;
    VFat_Put32(pe + 8, VFAT_PART_LBA);
    VFat_Put32(pe + 12, VFAT_PART_SECTORS);
    b[510] = 0x55; b[511] = 0xAA;
}

static void VFat_Boot(const VFat *v, uint8_t *b)
{
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    VFat_Put16(b + 11, VFAT_SECTOR);
    b[13] = VFAT_SPC;
    VFat_Put16(b + 14, VFAT_RSVD);
    b[16] = VFAT_NFATS;
    VFat_Put16(b + 17, VFAT_ROOT_ENTS);
    VFat_Put16(b + 19, VFAT_PART_SECTORS);          /* < 65536 — в 16-битном поле */
    b[21] = 0xF8;
    VFat_Put16(b + 22, VFAT_FAT_SECTORS);
    VFat_Put16(b + 24, 63);
    VFat_Put16(b + 26, 255);
    VFat_Put32(b + 28, VFAT_PART_LBA);
    b[36] = 0x80;
    b[38] = 0x29;
    VFat_Put32(b + 39, v->serial);
    memcpy(b + 43, "FIELDSENSOR", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

/* Сектор j таблицы FAT: записи кластеров 256j..256j+255. */
static void VFat_FatSector(const VFat *v, uint32_t j, uint8_t *b)
{
    for (uint32_t e = 0; e < VFAT_SECTOR / 2U; e++)
    {
        uint32_t c = j * (VFAT_SECTOR / 2U) + e, val = 0;
        if (c == 0U)
            val = 0xFFF8U;                      /* носитель F8 */
        else if (c == 1U)
            val = 0xFFFFU;                      /* том чист */
        else
            for (uint32_t f = 0; f < VFAT_FILES; f++)
                if (v->ncl[f] && c >= v->clus[f] && c < (uint32_t)v->clus[f] + v->ncl[f])
                    val = (c + 1U == (uint32_t)v->clus[f] + v->ncl[f]) ? 0xFFFFU : c + 1U;
        VFat_Put16(b + e * 2U, val);
    }
}

static void VFat_DirEntry(const VFat *v, uint8_t *e, const char *name83, uint8_t attr,
                          uint32_t f)
{
    memcpy(e, name83, 11);
    e[11] = attr;
    /* Имя и расширение строчными (бит NT, как пишет Windows и FatFs):
     * хост покажет "data.txt", а не "DATA.TXT". */
    e[12] = (attr == 0x08U) ? 0x00U : 0x18U;
    VFat_Put16(e + 14, v->ftime);
    VFat_Put16(e + 16, v->fdate);
    VFat_Put16(e + 18, v->fdate);
    VFat_Put16(e + 22, v->ftime);
    VFat_Put16(e + 24, v->fdate);
    if (f < VFAT_FILES)
    {
        VFat_Put16(e + 26, v->clus[f]);
        VFat_Put32(e + 28, v->size[f]);
    }
}

static void VFat_Root(const VFat *v, uint8_t *b)
{
    static const char *const NAMES[VFAT_FILES] = { "DATA    TXT", "DATA_OLDTXT", "INFO    TXT" };
    uint8_t *e = b;
    VFat_DirEntry(v, e, "FIELDSENSOR", 0x08, VFAT_FILES);
    e += 32;
    for (uint32_t f = 0; f < VFAT_FILES; f++)
        if (v->present[f])
        {
            /* Архив (0x20), но не «только чтение»: этот бит Windows
             * переносит на копию, и заказчик не смог бы её править. От
             * записи защищён сам том. */
            VFat_DirEntry(v, e, NAMES[f], 0x20, f);
            e += 32;
        }
}

uint8_t VFat_Read(VFat *v, uint32_t lba, uint8_t *buf)
{
    memset(buf, 0, VFAT_SECTOR);
    if (lba == 0U)
    {
        VFat_Mbr(buf);
        return 1;
    }
    if (lba < VFAT_PART_LBA || lba >= VFAT_DISK_SECTORS)
        return 1;
    uint32_t s = lba - VFAT_PART_LBA;
    if (s == 0U)
        VFat_Boot(v, buf);
    else if (s >= VFAT_FAT1 && s < VFAT_ROOT)
        VFat_FatSector(v, (s - VFAT_FAT1) % VFAT_FAT_SECTORS, buf);
    else if (s == VFAT_ROOT)
        VFat_Root(v, buf);
    else if (s >= VFAT_DATA)
    {
        uint32_t c = (s - VFAT_DATA) / VFAT_SPC + 2U;
        for (uint32_t f = 0; f < VFAT_FILES; f++)
        {
            if (!v->ncl[f] || c < v->clus[f] || c >= (uint32_t)v->clus[f] + v->ncl[f])
                continue;
            uint32_t x = (c - v->clus[f]) * VFAT_CLUSTER + ((s - VFAT_DATA) % VFAT_SPC) * VFAT_SECTOR;
            if (x >= v->size[f])
                return 1;                       /* хвост последнего кластера */
            uint32_t len = v->size[f] - x;
            if (len > VFAT_SECTOR)
                len = VFAT_SECTOR;
            if (f == VFAT_FILE_INFO)
            {
                char info[VFAT_SECTOR];
                (void)VFat_RenderInfo(v, info, sizeof(info));
                memcpy(buf, info, len);
                return 1;
            }
            return VFat_FileRead(v, f, x, buf, len);
        }
    }
    return 1;
}

#endif /* VFAT_IMPLEMENTATION */
#endif
