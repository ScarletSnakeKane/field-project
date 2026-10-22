#ifndef LOG_RECORD_H
#define LOG_RECORD_H

/* Двоичная запись кольцевого журнала (П5а) и форматтер строки CSV.
 *
 * Без HAL и без регистров — чтобы проверять хостовыми тестами. Всё —
 * static inline, отдельного .c нет: Debug/ не в git, и сборка CubeIDE новый
 * файл не увидит.
 *
 * Размер записи — 64 байта. Он делит страницу флеш (256 Б) нацело, так что
 * запись никогда не пересекает границу страницы и программируется одной
 * командой Page Program. 128 байт были бы просторнее, но тогда в 2 МБ
 * помещается 1.86 года часовых замеров, а гарантия G6 — 3 года; 64 байта
 * дают 3.7 года (расчёт — в отчёте П5а). Плата за это — упаковка: мелкие
 * коды — битами, сырые отсчёты АЦП — в uint16. Всё, что прошивка реально
 * может записать, восстанавливается без потерь (тест сверяет вывод
 * форматтера с эталонными строками старого snprintf из main.c).
 *
 * Раскладка (little-endian, байты, а не struct: упаковка структур у gcc для
 * ARM и у хостового компилятора могла бы разойтись):
 *    0      тип записи (LOG_TYPE_*); 0xFF — стёрто, 0x00 — не наш тип
 *    1      версия формата (LOG_VERSION)
 *    2..5   seq — номер записи; растёт на 1 на каждый занятый слот журнала
 *    6..59  полезная нагрузка, 54 байта, зависит от типа
 *    60..63 CRC32 (IEEE 802.3, как у zip) по байтам 0..59 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define LOG_RECORD_SIZE   64U
#define LOG_PAYLOAD_OFS   6U
#define LOG_PAYLOAD_SIZE  54U
#define LOG_CRC_OFS       60U
#define LOG_VERSION       1U

/* Типы — заметные буквы, а не 0/1: стёртая флеш (0xFF) и обнулённая (0x00)
 * типом записи не бывают никогда. */
#define LOG_TYPE_MEAS         0x4DU   /* 'M' — замер, одна строка CSV */
#define LOG_TYPE_TEXT         0x54U   /* 'T' — кусок текста старого архива */
#define LOG_TYPE_IMPORT_FILE  0x46U   /* 'F' — начало файла старого архива */
#define LOG_TYPE_IMPORT_END   0x45U   /* 'E' — импорт старого архива закончен */

/* Набор колонок CSV — тот же, что раньше стоял в main.c (описание колонок —
 * там же, над местом записи). Без перевода строки: его добавляет тот, кто
 * собирает файл. */
#define LOG_CSV_HEADER "timestamp,air_temp,air_hum,soil_temp,soil_hum," \
                       "bat_pct,vbat,vdd,soil_valid,lse_ok,time_ok," \
                       "aht_init_st,aht_read_st,ds_err,air_xchk,i2c_rec,sens_off," \
                       "write_fails,lowpower_skips,reset_cause,boot_count," \
                       "soil_raw,bat_raw"

/* Строка CSV не длиннее этого (с "\n" и нулём): 23 поля при худших значениях
 * — около 200 символов, как и буфер line[224] прежнего кода. */
#define LOG_CSV_LINE_MAX  224U

typedef struct {
    uint8_t b[LOG_RECORD_SIZE];
} LogRecord;

/* Замер в том виде, в каком его собирает main.c, — те же переменные, что
 * раньше уходили в snprintf. Форматтер вычисляет из них то же, что прежнее
 * выражение (nan при отказе батареи, soil_valid = valid && !low). */
typedef struct {
    char     ts[20];          /* "YYYY-MM-DD hh:mm:ss" от Time_GetTimestamp */
    float    air_temp, air_hum;
    float    soil_temp, soil_hum;
    uint8_t  bat_valid, bat_low, bat_percent;
    float    bat_volts, vdd;
    uint8_t  lse_ok, time_ok;
    int      aht_init_st, aht_read_st;   /* HAL_StatusTypeDef: 0..3 */
    uint8_t  ds_err, air_xchk, i2c_rec;
    uint32_t sens_off, write_fails, lowpower_skips;
    uint16_t reset_cause;
    uint32_t boot_count;
    float    soil_raw, bat_raw;           /* отсчёты АЦП 0..4095 или NAN */
} LogMeas;

/* ------------------------------------------------------------ байты и CRC */

static inline void LogRecord_Put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static inline void LogRecord_Put32(uint8_t *p, uint32_t v)
{
    LogRecord_Put16(p, v); LogRecord_Put16(p + 2, v >> 16);
}

static inline uint32_t LogRecord_Get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t LogRecord_Get32(const uint8_t *p)
{
    return LogRecord_Get16(p) | (LogRecord_Get16(p + 2) << 16);
}

static inline void LogRecord_PutF(uint8_t *p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    LogRecord_Put32(p, v);
}

static inline float LogRecord_GetF(const uint8_t *p)
{
    uint32_t v = LogRecord_Get32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* CRC32 (zip, отражённый 0xEDB88320) по полубайтам. Сборка тома для USB
 * (vfat.h) проверяет CRC каждой из до 32 тыс. записей, и побитовый CRC был
 * почти половиной её времени. Таблица на 16 полубайтов — 64 Б флеш в каждой
 * единице трансляции (static inline: main.c, spiflash.c, usbd_storage_if.c
 * — 192 Б), в 3.7 раза быстрее побитового. Таблица на 256 быстрее ещё в 1.5
 * раза, но это 3 КБ, а сборку тома ускорила бы лишь на 7 % (замеры — отчёт
 * П5б). Значения CRC те же. */
static inline uint32_t LogRecord_Crc32(const uint8_t *p, uint32_t n)
{
    static const uint32_t t[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL };
    uint32_t c = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < n; i++)
    {
        c ^= p[i];
        c = (c >> 4) ^ t[c & 15U];
        c = (c >> 4) ^ t[c & 15U];
    }
    return ~c;
}

static inline uint8_t  LogRecord_Type(const LogRecord *r) { return r->b[0]; }
static inline uint32_t LogRecord_Seq(const LogRecord *r)  { return LogRecord_Get32(r->b + 2); }

/* Проставить номер и CRC — последний шаг перед программированием. */
static inline void LogRecord_Seal(LogRecord *r, uint32_t seq)
{
    LogRecord_Put32(r->b + 2, seq);
    LogRecord_Put32(r->b + LOG_CRC_OFS, LogRecord_Crc32(r->b, LOG_CRC_OFS));
}

#define LOG_REC_ERASED  0U   /* все 64 байта 0xFF — слот свободен */
#define LOG_REC_OK      1U
#define LOG_REC_BAD     2U   /* недописана, побита или чужая */

static inline uint8_t LogRecord_IsErased(const LogRecord *r)
{
    for (uint32_t i = 0; i < LOG_RECORD_SIZE; i++)
        if (r->b[i] != 0xFF)
            return 0;
    return 1;
}

/* Годна ли запись. Тип и версия проверяются вдобавок к CRC: запись будущей
 * версии формата эта прошивка всё равно не разберёт. */
static inline uint8_t LogRecord_Check(const LogRecord *r)
{
    if (LogRecord_IsErased(r))
        return LOG_REC_ERASED;
    if (LogRecord_Get32(r->b + LOG_CRC_OFS) != LogRecord_Crc32(r->b, LOG_CRC_OFS))
        return LOG_REC_BAD;
    uint8_t t = r->b[0];
    if (r->b[1] != LOG_VERSION ||
        (t != LOG_TYPE_MEAS && t != LOG_TYPE_TEXT &&
         t != LOG_TYPE_IMPORT_FILE && t != LOG_TYPE_IMPORT_END))
        return LOG_REC_BAD;
    return LOG_REC_OK;
}

/* Может ли слот быть нашей записью, хотя бы недописанной. Программирование
 * NOR только сбрасывает биты 1 -> 0, и оборванное оставляет в байте типа
 * часть единиц: байт — надмножество (по единицам) одного из типов. Байт без
 * этого свойства — чужие данные (текст старого тома), а не наш обрыв. */
static inline uint8_t LogRecord_MayBeOurs(const LogRecord *r)
{
    static const uint8_t types[4] = { LOG_TYPE_MEAS, LOG_TYPE_TEXT,
                                      LOG_TYPE_IMPORT_FILE, LOG_TYPE_IMPORT_END };
    for (uint32_t i = 0; i < 4; i++)
        if ((r->b[0] & types[i]) == types[i])
            return 1;
    return 0;
}

static inline void LogRecord_Begin(LogRecord *r, uint8_t type)
{
    memset(r->b, 0, sizeof(r->b));
    r->b[0] = type;
    r->b[1] = LOG_VERSION;
}

/* ------------------------------------------------------------------ замер
 *
 * Нагрузка замера (смещения от LOG_PAYLOAD_OFS):
 *    0..5   метка: год-2000, месяц, день, час, минута, секунда (по байту)
 *    6..29  float: air_temp, air_hum, soil_temp, soil_hum, bat_volts, vdd
 *    30     bat_percent
 *    31..34 uint16: soil_raw, bat_raw (0xFFFF — NAN)
 *    35..36 uint16 битами: bat_valid(0) bat_low(1) lse_ok(2) time_ok(3)
 *           aht_init_st(4..5) aht_read_st(6..7) ds_err(8..11)
 *           air_xchk(12..13) i2c_rec(14..15)
 *    37..38 uint16 sens_off
 *    39..40 uint16 reset_cause
 *    41..52 uint32: write_fails, lowpower_skips, boot_count
 *    53     резерв (0)
 *
 * Где упаковка теряет: значения шире своего поля насыщаются до максимума
 * поля. Прошивка таких не порождает: коды HAL — 0..3, ds_err — до 8,
 * air_xchk и i2c_rec — до 3, флаги — 0/1, sens_off — не больше
 * SENSOR_QUARANTINE_CYCLES (6 при часовом интервале, 360 на стенде),
 * сырые отсчёты — 12-битный АЦП. Метка — фиксированной ширины при любых
 * регистрах RTC: BCD-поля дают не больше двух цифр, год — 2000..2165. */

static inline uint32_t LogRecord_Sat(uint32_t v, uint32_t max)
{
    return (v > max) ? max : v;
}

static inline uint32_t LogRecord_Code(int v, uint32_t max)
{
    return (v < 0) ? max : LogRecord_Sat((uint32_t)v, max);
}

static inline uint32_t LogRecord_PackRaw(float v)
{
    if (isnan(v) || v < 0.0f)
        return 0xFFFFU;
    if (v > 65534.0f)
        return 65534U;
    return (uint32_t)lroundf(v);
}

static inline float LogRecord_UnpackRaw(uint32_t v)
{
    return (v == 0xFFFFU) ? NAN : (float)v;
}

/* Две десятичные цифры; не цифра — 0xFF (метка не разобрана). */
static inline uint32_t LogRecord_Digits(const char *p, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        if (p[i] < '0' || p[i] > '9')
            return 0xFFFFFFFFUL;
        v = v * 10U + (uint32_t)(p[i] - '0');
    }
    return v;
}

#define LOG_TS_UNKNOWN "xxxx-xx-xx xx:xx:xx"

static inline void LogRecord_PackMeas(LogRecord *r, const LogMeas *m)
{
    LogRecord_Begin(r, LOG_TYPE_MEAS);
    uint8_t *p = r->b + LOG_PAYLOAD_OFS;

    /* Метка "YYYY-MM-DD hh:mm:ss" — по позициям, как её печатает
     * Time_GetTimestamp; год — от 2000, остальное — по байту. Неразобранная
     * метка (не та ширина, не цифры) пишется 0xFF во всех полях, и форматтер
     * печатает её как LOG_TS_UNKNOWN: лучше заметная дыра, чем правдоподобная
     * дата. */
    static const uint8_t pos[6] = { 0, 5, 8, 11, 14, 17 };
    uint32_t year = LogRecord_Digits(m->ts, 4);
    uint8_t  bad  = (year < 2000U || year > 2254U ||
                     m->ts[4] != '-' || m->ts[7] != '-' || m->ts[10] != ' ' ||
                     m->ts[13] != ':' || m->ts[16] != ':' || m->ts[19] != '\0');
    for (uint32_t i = 1; i < 6 && !bad; i++)
        if (LogRecord_Digits(m->ts + pos[i], 2) > 99U)
            bad = 1;
    p[0] = bad ? 0xFF : (uint8_t)(year - 2000U);
    for (uint32_t i = 1; i < 6; i++)
        p[i] = bad ? 0xFF : (uint8_t)LogRecord_Digits(m->ts + pos[i], 2);

    LogRecord_PutF(p + 6,  m->air_temp);
    LogRecord_PutF(p + 10, m->air_hum);
    LogRecord_PutF(p + 14, m->soil_temp);
    LogRecord_PutF(p + 18, m->soil_hum);
    LogRecord_PutF(p + 22, m->bat_volts);
    LogRecord_PutF(p + 26, m->vdd);
    p[30] = m->bat_percent;
    LogRecord_Put16(p + 31, LogRecord_PackRaw(m->soil_raw));
    LogRecord_Put16(p + 33, LogRecord_PackRaw(m->bat_raw));

    uint32_t bits = (m->bat_valid ? 1U : 0U)
                  | (m->bat_low   ? 2U : 0U)
                  | (m->lse_ok    ? 4U : 0U)
                  | (m->time_ok   ? 8U : 0U)
                  | (LogRecord_Code(m->aht_init_st, 3U) << 4)
                  | (LogRecord_Code(m->aht_read_st, 3U) << 6)
                  | (LogRecord_Sat(m->ds_err, 15U)   << 8)
                  | (LogRecord_Sat(m->air_xchk, 3U)  << 12)
                  | (LogRecord_Sat(m->i2c_rec, 3U)   << 14);
    LogRecord_Put16(p + 35, bits);
    LogRecord_Put16(p + 37, LogRecord_Sat(m->sens_off, 0xFFFFU));
    LogRecord_Put16(p + 39, m->reset_cause);
    LogRecord_Put32(p + 41, m->write_fails);
    LogRecord_Put32(p + 45, m->lowpower_skips);
    LogRecord_Put32(p + 49, m->boot_count);
}

static inline void LogRecord_UnpackMeas(const LogRecord *r, LogMeas *m)
{
    const uint8_t *p = r->b + LOG_PAYLOAD_OFS;

    uint8_t bad = (p[0] == 0xFF);
    for (uint32_t i = 1; i < 6; i++)
        if (p[i] > 99U)
            bad = 1;
    if (bad)
        memcpy(m->ts, LOG_TS_UNKNOWN, sizeof(LOG_TS_UNKNOWN));
    else
        snprintf(m->ts, sizeof(m->ts), "%04u-%02u-%02u %02u:%02u:%02u",
                 2000U + p[0], p[1] % 100U, p[2] % 100U, p[3] % 100U, p[4] % 100U, p[5] % 100U);

    m->air_temp  = LogRecord_GetF(p + 6);
    m->air_hum   = LogRecord_GetF(p + 10);
    m->soil_temp = LogRecord_GetF(p + 14);
    m->soil_hum  = LogRecord_GetF(p + 18);
    m->bat_volts = LogRecord_GetF(p + 22);
    m->vdd       = LogRecord_GetF(p + 26);
    m->bat_percent = p[30];
    m->soil_raw  = LogRecord_UnpackRaw(LogRecord_Get16(p + 31));
    m->bat_raw   = LogRecord_UnpackRaw(LogRecord_Get16(p + 33));

    uint32_t bits = LogRecord_Get16(p + 35);
    m->bat_valid   = (uint8_t)(bits & 1U);
    m->bat_low     = (uint8_t)((bits >> 1) & 1U);
    m->lse_ok      = (uint8_t)((bits >> 2) & 1U);
    m->time_ok     = (uint8_t)((bits >> 3) & 1U);
    m->aht_init_st = (int)((bits >> 4) & 3U);
    m->aht_read_st = (int)((bits >> 6) & 3U);
    m->ds_err      = (uint8_t)((bits >> 8) & 15U);
    m->air_xchk    = (uint8_t)((bits >> 12) & 3U);
    m->i2c_rec     = (uint8_t)((bits >> 14) & 3U);
    m->sens_off    = LogRecord_Get16(p + 37);
    m->reset_cause = (uint16_t)LogRecord_Get16(p + 39);
    m->write_fails    = LogRecord_Get32(p + 41);
    m->lowpower_skips = LogRecord_Get32(p + 45);
    m->boot_count     = LogRecord_Get32(p + 49);
}

/* Строка CSV замера — тот самый snprintf, что раньше стоял в main.c, с теми
 * же выражениями над теми же значениями: формат файла не меняется ни на
 * байт. Перевод строки — один "\n", как и раньше: на диск старый код писал
 * "\r\n", потому что f_puts разворачивал \n сам; том П5б делает так же.
 * Возвращает длину строки или -1 (не замер, мал буфер). */
static inline int LogRecord_FormatMeas(const LogMeas *m, char *out, size_t size)
{
    int n = snprintf(out, size,
             "%s,%.2f,%.2f,%.2f,%.2f,%.0f,%.2f,%.3f,%u,%u,%u,%d,%d,%u,%u,%u,%lu,"
             "%lu,%lu,%u,%lu,%.0f,%.0f\n",
             m->ts,
             (double)m->air_temp,
             (double)m->air_hum,
             (double)m->soil_temp,
             (double)m->soil_hum,
             /* Отказ замера батареи — nan в обеих колонках, как у остальных
              * датчиков (а не "0,0.00" — «батарея пуста»). */
             m->bat_valid ? (double)m->bat_percent : (double)NAN,
             m->bat_valid ? (double)m->bat_volts   : (double)NAN,
             (double)m->vdd,
             /* soil_valid: ниже дропаута LDO опора АЦП уплывает вместе с
              * банкой, и влажность почвы с этого момента недостоверна. */
             (unsigned)(m->bat_valid && !m->bat_low),
             (unsigned)m->lse_ok,
             (unsigned)m->time_ok,
             m->aht_init_st,
             m->aht_read_st,
             (unsigned)m->ds_err,
             (unsigned)m->air_xchk,
             (unsigned)m->i2c_rec,
             (unsigned long)m->sens_off,
             (unsigned long)m->write_fails,
             (unsigned long)m->lowpower_skips,
             (unsigned)m->reset_cause,
             (unsigned long)m->boot_count,
             (double)m->soil_raw,
             (double)m->bat_raw);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

static inline int LogRecord_FormatCsv(const LogRecord *r, char *out, size_t size)
{
    if (size > 0)
        out[0] = '\0';
    if (LogRecord_Type(r) != LOG_TYPE_MEAS)
        return -1;
    LogMeas m;
    LogRecord_UnpackMeas(r, &m);
    return LogRecord_FormatMeas(&m, out, size);
}

/* ------------------------------------------------- текст старого архива
 *
 * TEXT:        0 — номер файла в импорте, 1 — длина (1..52), 2..53 — байты
 *              текста как есть (с \r\n, заголовками и всем прочим).
 * IMPORT_FILE: 0 — номер файла, 1..4 — размер файла, 5..17 — имя 8.3 с
 *              нулём ("data_3.txt"), 18..21 — сколько байт начала файла не
 *              влезло (тогда перенесены первая строка и хвост). Открывает
 *              каждый файл.
 * IMPORT_END:  0 — сколько файлов, 1..4 — сколько байт текста всего,
 *              5 — сколько файлов не влезло или не прочлось.
 *              Пока его нет, импорт не закончен (см. log_ring.h). */
#define LOG_TEXT_MAX       52U
#define LOG_IMPORT_NAME    13U

static inline void LogRecord_MakeText(LogRecord *r, uint8_t file, const uint8_t *text, uint32_t len)
{
    LogRecord_Begin(r, LOG_TYPE_TEXT);
    if (len > LOG_TEXT_MAX)
        len = LOG_TEXT_MAX;
    r->b[LOG_PAYLOAD_OFS]     = file;
    r->b[LOG_PAYLOAD_OFS + 1] = (uint8_t)len;
    memcpy(r->b + LOG_PAYLOAD_OFS + 2, text, len);
}

/* Текст записи TEXT: указатель на байты и длина; 0 — не TEXT или битая длина. */
static inline uint32_t LogRecord_Text(const LogRecord *r, const uint8_t **text, uint8_t *file)
{
    uint32_t len = r->b[LOG_PAYLOAD_OFS + 1];
    if (LogRecord_Type(r) != LOG_TYPE_TEXT || len == 0 || len > LOG_TEXT_MAX)
        return 0;
    if (text) *text = r->b + LOG_PAYLOAD_OFS + 2;
    if (file) *file = r->b[LOG_PAYLOAD_OFS];
    return len;
}

static inline void LogRecord_MakeImportFile(LogRecord *r, uint8_t file, uint32_t size, const char *name)
{
    LogRecord_Begin(r, LOG_TYPE_IMPORT_FILE);
    r->b[LOG_PAYLOAD_OFS] = file;
    LogRecord_Put32(r->b + LOG_PAYLOAD_OFS + 1, size);
    size_t n = strlen(name);
    if (n > LOG_IMPORT_NAME - 1U)
        n = LOG_IMPORT_NAME - 1U;
    memcpy(r->b + LOG_PAYLOAD_OFS + 5, name, n);
}

static inline void LogRecord_MakeImportEnd(LogRecord *r, uint8_t files, uint32_t bytes)
{
    LogRecord_Begin(r, LOG_TYPE_IMPORT_END);
    r->b[LOG_PAYLOAD_OFS] = files;
    LogRecord_Put32(r->b + LOG_PAYLOAD_OFS + 1, bytes);
}

#endif
