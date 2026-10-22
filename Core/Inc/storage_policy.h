#ifndef STORAGE_POLICY_H
#define STORAGE_POLICY_H

/* Решения о том, когда трогать архив на флеш. Без HAL и без регистров —
 * чтобы их можно было проверить хостовыми тестами. Отдельный .c не заведён
 * сознательно: Debug/ не в git, и сборка CubeIDE новый файл не увидит. */

#include <stdint.h>
#include <math.h>
#include <string.h>

/* Логический сектор FAT и место загрузочного сектора раздела. f_mkfs без
 * FM_SFD кладёт раздел с LBA 63 (ff.c, b_vol), MBR — в сектор 0. */
#define STORAGE_SECTOR_BYTES   512U
#define STORAGE_BOOT_SECTOR    63U

/* Ниже этого VDD стирание и программирование SST26VF016B идут вне паспорта
 * (2.7–3.6 В), а пик тока стирания на исходе банки может уронить питание
 * посреди RMW блока каталога или FAT. Запас 0.2 В над паспортом — на просадку
 * под нагрузкой, которую VREFINT в момент замера не видит. */
#define STORAGE_MIN_VDD_VOLTS  2.9f

/* Весь буфер — 0xFF, то есть стёртая флеш. */
static inline uint8_t Storage_IsErased(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != 0xFF)
            return 0;
    return 1;
}

/* Можно ли форматировать том. Раньше хватало FR_NO_FILESYSTEM от f_mount —
 * то есть одного искажённого чтения сектора 0 или 63, чтобы f_mkfs стёр архив
 * за месяцы. Теперь форматируется только флеш, на которой заведомо нечего
 * терять: оба сектора прочитаны без ошибки и стёрты целиком. Любой байт не
 * 0xFF означает, что там что-то лежало, — пусть битое, но разбирать его
 * будет человек, а не автоформат.
 *   no_fs    — f_mount вернул FR_NO_FILESYSTEM;
 *   reads_ok — оба сектора прочитаны без ошибки драйвера. */
static inline uint8_t Storage_MayFormat(uint8_t no_fs, uint8_t reads_ok,
                                        const uint8_t *mbr, const uint8_t *boot)
{
    if (!no_fs || !reads_ok)
        return 0;
    return Storage_IsErased(mbr, STORAGE_SECTOR_BYTES) &&
           Storage_IsErased(boot, STORAGE_SECTOR_BYTES);
}

/* LBA первого сектора корневого каталога, если boot — загрузочный сектор,
 * который кладёт наш f_mkfs (FAT12/16, раздел с LBA 63, OEM "MSDOS5.0");
 * иначе 0. Корень лежит сразу за резервной областью и таблицами FAT —
 * в том же порядке f_mkfs их и пишет (ff.c, «Initialize FAT area», затем
 * «Initialize root directory»). */
static inline uint32_t Storage_OurRootDirSector(const uint8_t *boot)
{
    static const uint8_t jmp_oem[11] = {0xEB, 0xFE, 0x90,
                                        'M', 'S', 'D', 'O', 'S', '5', '.', '0'};
    if (memcmp(boot, jmp_oem, sizeof(jmp_oem)) != 0)
        return 0;
    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return 0;

    uint32_t bps   = (uint32_t)boot[11] | ((uint32_t)boot[12] << 8);
    uint32_t rsvd  = (uint32_t)boot[14] | ((uint32_t)boot[15] << 8);
    uint32_t nfats = boot[16];
    uint32_t fatsz = (uint32_t)boot[22] | ((uint32_t)boot[23] << 8);
    uint32_t hidd  = (uint32_t)boot[28] | ((uint32_t)boot[29] << 8) |
                     ((uint32_t)boot[30] << 16) | ((uint32_t)boot[31] << 24);

    if (bps != STORAGE_SECTOR_BYTES || hidd != STORAGE_BOOT_SECTOR)
        return 0;
    if (rsvd == 0 || nfats == 0 || nfats > 2 || fatsz == 0)
        return 0;
    return STORAGE_BOOT_SECTOR + rsvd + nfats * fatsz;
}

/* В секторе каталога нет ни одной записи: только нули (f_mkfs его очистил)
 * или 0xFF (до очистки не дошёл). Удалённый файл (0xE5) — уже запись: файл
 * когда-то был, и решать о нём будет человек. */
static inline uint8_t Storage_IsEmptyDir(const uint8_t *dir)
{
    for (uint32_t i = 0; i < STORAGE_SECTOR_BYTES; i++)
        if (dir[i] != 0x00 && dir[i] != 0xFF)
            return 0;
    return 1;
}

/* Можно ли доделать сорванный формат (R3). f_mkfs пишет загрузочный сектор
 * первым, а MBR — последним; если между ними пропало питание или сверка
 * блока не сошлась, на флеш остаётся наш загрузочный сектор при стёртом MBR.
 * Storage_MayFormat такую флеш чистой не считает, и прибор навсегда
 * оставался бы с fs_ok = 0. Доформатировать безопасно, только если на томе
 * не было ни одного файла: тем же «MBR стёрт, загрузочный жив» выглядит и
 * прерванная правка типа раздела в MBR на томе с архивом — у того в корне
 * лежит data.txt.
 *   root_ok — корневой сектор прочитан без ошибки драйвера. */
static inline uint8_t Storage_MayRedoFormat(uint8_t no_fs, uint8_t reads_ok, uint8_t root_ok,
                                            const uint8_t *mbr, const uint8_t *boot,
                                            const uint8_t *root)
{
    if (!no_fs || !reads_ok || !root_ok)
        return 0;
    if (!Storage_IsErased(mbr, STORAGE_SECTOR_BYTES))
        return 0;
    if (Storage_OurRootDirSector(boot) == 0)
        return 0;
    return Storage_IsEmptyDir(root);
}

/* Тип раздела, который наша прошивка держит в MBR для FAT16: 0x0E (FAT16
 * LBA) — его Android монтирует, а 0x04 от f_mkfs пропускает (см. main.c). */
#define STORAGE_MBR_TYPE_FAT16_LBA  0x0EU
#define STORAGE_MBR_TABLE           446U

static inline uint32_t Storage_Ld16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t Storage_Ld32(const uint8_t *p)
{
    return Storage_Ld16(p) | (Storage_Ld16(p + 2) << 16);
}

/* Размер тома в секторах по BPB: 16-битное поле, а если оно ноль — 32-битное. */
static inline uint32_t Storage_BootTotalSectors(const uint8_t *boot)
{
    uint32_t tot = Storage_Ld16(boot + 19);
    return tot ? tot : Storage_Ld32(boot + 32);
}

/* Можно ли восстановить стёртый MBR по загрузочному сектору (решение
 * владельца по остатку R3). Так выглядит прерванная правка байта типа
 * раздела (main.c, RMW блока 0) на томе с архивом: MBR стёрт целиком, а
 * загрузочный сектор раздела жив. Без MBR FatFs тома не видит, прибор
 * навсегда остаётся с fs_ok = 0, а хост не видит раздела, хотя архив цел.
 *
 * MBR, который кладёт f_mkfs, однозначно выводится из загрузочного сектора
 * (ff.c, «Create partition table»): один раздел с LBA 63, размер — из BPB,
 * тип — по числу кластеров. Восстанавливаем только то, в чём уверены:
 * MBR стёрт целиком (любой байт не 0xFF — там что-то другое, разбирать
 * человеку), загрузочный сектор узнаётся как наш, а том помещается на диск.
 *   disk_sectors — сколько секторов отдаёт драйвер (GET_SECTOR_COUNT). */
static inline uint8_t Storage_MayRestoreMbr(uint8_t no_fs, uint8_t reads_ok,
                                            const uint8_t *mbr, const uint8_t *boot,
                                            uint32_t disk_sectors)
{
    if (!no_fs || !reads_ok)
        return 0;
    if (!Storage_IsErased(mbr, STORAGE_SECTOR_BYTES))
        return 0;
    if (Storage_OurRootDirSector(boot) == 0)
        return 0;
    uint32_t tot = Storage_BootTotalSectors(boot);
    if (tot == 0 || tot > disk_sectors || STORAGE_BOOT_SECTOR > disk_sectors - tot)
        return 0;
    return 1;
}

/* MBR для тома, описанного загрузочным сектором boot, — байт в байт как
 * f_mkfs (ff.c около 5727–5742), только тип FAT16 сразу 0x0E, как его
 * оставляет наша прошивка. Вызывать после Storage_MayRestoreMbr. */
static inline void Storage_BuildMbr(const uint8_t *boot, uint8_t *mbr)
{
    uint32_t tot   = Storage_BootTotalSectors(boot);
    uint32_t spc   = boot[13];
    uint32_t rsvd  = Storage_Ld16(boot + 14);
    uint32_t nfats = boot[16];
    uint32_t rootn = Storage_Ld16(boot + 17);
    uint32_t fatsz = Storage_Ld16(boot + 22);
    uint32_t meta  = rsvd + nfats * fatsz + (rootn * 32U + STORAGE_SECTOR_BYTES - 1U) / STORAGE_SECTOR_BYTES;
    uint32_t nclst = (spc && tot > meta) ? (tot - meta) / spc : 0;
    uint8_t  sys;

    /* Та же граница FAT12/FAT16, по которой FatFs различает тип при
     * монтировании (MAX_FAT12 = 0xFF5). */
    if (tot >= 0x10000UL)
        sys = 0x06;
    else
        sys = (nclst > 0xFF5U) ? STORAGE_MBR_TYPE_FAT16_LBA : 0x01;

    memset(mbr, 0, STORAGE_SECTOR_BYTES);
    uint8_t *pte = mbr + STORAGE_MBR_TABLE;
    uint32_t n = (STORAGE_BOOT_SECTOR + tot) / (63U * 255U);
    pte[0] = 0;                          /* не загрузочный */
    pte[1] = 1;  pte[2] = 1;  pte[3] = 0; /* начало CHS */
    pte[4] = sys;
    pte[5] = 254;                        /* конец CHS — как у f_mkfs */
    pte[6] = (uint8_t)(n >> 2 | 63);
    pte[7] = (uint8_t)n;
    pte[8]  = (uint8_t)STORAGE_BOOT_SECTOR;
    pte[9]  = 0; pte[10] = 0; pte[11] = 0;
    pte[12] = (uint8_t)tot;         pte[13] = (uint8_t)(tot >> 8);
    pte[14] = (uint8_t)(tot >> 16); pte[15] = (uint8_t)(tot >> 24);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

/* Что делать со старым data.txt при старте (A11/C5). Раньше любое отличие
 * первой строки от CSV_HEADER — в том числе сорванное чтение — вело к
 * f_unlink: перепрошивка с новой колонкой молча стирала архив за полгода.
 * Теперь архив не удаляется никогда: другой заголовок — файл переименуется
 * (Storage_PickOldArchiveName) и начнётся новый. Сорванное чтение — не
 * «другой заголовок»: файл не трогаем и дописываем как есть. Пустой файл
 * (f_gets ничего не прочёл) тоже не трогаем — заголовок допишет первая же
 * дозапись.
 *   read_ok — f_gets вернул строку;
 *   line    — прочитанная первая строка (f_gets снимает \r, оставляет \n). */
#define STORAGE_HDR_KEEP    0U
#define STORAGE_HDR_RENAME  1U

static inline uint8_t Storage_HeaderAction(uint8_t read_ok, const char *line,
                                           const char *header)
{
    if (!read_ok)
        return STORAGE_HDR_KEEP;

    /* Совпасть должна строка целиком: заголовок более новой прошивки
     * начинается с нашего, и сравнение по префиксу смешало бы форматы. */
    size_t n = strlen(header);
    if (strncmp(line, header, n) != 0)
        return STORAGE_HDR_RENAME;
    char c = line[n];
    return (c == '\0' || c == '\n' || c == '\r') ? STORAGE_HDR_KEEP : STORAGE_HDR_RENAME;
}

/* Имя для старого архива: data_N.txt с первым свободным N от 1. Имена 8.3 —
 * FatFs собран без длинных имён, поэтому N не больше 999. taken(name, ctx)
 * — занято ли имя (в прошивке — f_stat; ошибка тома — STORAGE_NAME_ERROR:
 * ничего не переименовываем и ничего не затираем). Возвращает N, а имя
 * кладёт в out (13 байт); 0 — свободных имён нет, out пуст. */
#define STORAGE_OLD_ARCHIVE_MAX  999U

/* Ответ taken: 0 — имя свободно, 1 — занято, 2 — том не ответил: перебор
 * прекращается (на сбоящей флеш каждый f_stat ждёт секундами, и 999 таких
 * вызовов без пинка сторожа кончились бы сбросом на каждом старте). */
#define STORAGE_NAME_FREE   0U
#define STORAGE_NAME_TAKEN  1U
#define STORAGE_NAME_ERROR  2U

typedef uint8_t (*Storage_NameTaken)(const char *name, void *ctx);

static inline void Storage_OldArchiveName(uint32_t n, char *out)
{
    /* Без snprintf: формат тривиален, а заголовку незачем тянуть stdio. */
    char digits[4];
    uint32_t len = 0;
    do { digits[len++] = (char)('0' + n % 10U); n /= 10U; } while (n && len < 3U);
    memcpy(out, "data_", 5);
    for (uint32_t i = 0; i < len; i++)
        out[5 + i] = digits[len - 1U - i];
    memcpy(out + 5 + len, ".txt", 5);
}

static inline uint32_t Storage_PickOldArchiveName(Storage_NameTaken taken, void *ctx, char *out)
{
    for (uint32_t n = 1; n <= STORAGE_OLD_ARCHIVE_MAX; n++)
    {
        Storage_OldArchiveName(n, out);
        uint8_t t = taken(out, ctx);
        if (t == STORAGE_NAME_FREE)
            return n;
        if (t != STORAGE_NAME_TAKEN)
            break;
    }
    out[0] = '\0';
    return 0;
}

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
 * задержки там не идут. Занята — основной поток посреди операции FatFs, и
 * чтение оборвало бы его кадр SPI или отдало хосту полузаписанный том.
 * В обоих случаях хост получает «не готов» и повторяет запрос сам. */
static inline uint8_t Storage_MscMayAccess(uint8_t flash_ready, uint8_t flash_busy)
{
    return (flash_ready && !flash_busy) ? 1U : 0U;
}

/* Можно ли сейчас писать на флеш при таком питании. Каждая дозапись — это
 * стирание и перепрограммирование 4-КБ блоков каталога и FAT, и сбой посреди
 * них теряет весь data.txt. Пропущенная строка стоит одного часа данных.
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

#endif
