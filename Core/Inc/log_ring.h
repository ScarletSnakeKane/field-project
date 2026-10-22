#ifndef LOG_RING_H
#define LOG_RING_H

/* Кольцевой журнал замеров на всей физической флеш (П5а).
 *
 * Single-header: здесь объявления, реализация — под LOG_RING_IMPLEMENTATION,
 * её включает ровно один .c прошивки (spiflash.c) и хостовые тесты. Флеш —
 * через таблицу функций LogFlashOps: в прошивке это SPI_Flash_*, на хосте —
 * модель NOR с обрывом питания.
 *
 * Устройство.
 *   Флеш делится на секторы стирания по 4 КБ, сектор — на 64 слота по
 *   64 байта (log_record.h). Запись идёт подряд по слотам, сектор за
 *   сектором, по кругу. Номер записи seq привязан к слоту: каждый занятый
 *   слот, даже испорченный обрывом, расходует один номер, так что в любом
 *   секторе seq слота k = seq слота 0 + k, а соседние секторы отличаются
 *   ровно на 64. Это и позволяет восстанавливаться, читая по одной записи
 *   на сектор, и находить i-ю запись от старейшей за O(1).
 *
 *   Пишется только программированием по стёртому. Сектор, следующий за
 *   сектором записи («запасной»), стирается заранее — сразу, как только
 *   запись перешла в новый сектор; стирание уносит старейшие 64 записи
 *   (G1). Поэтому запись в слот никогда не ждёт стирания своего же сектора,
 *   а обрыв посреди стирания портит только запасной сектор — старейший.
 *
 * Восстановление при старте (LogRing_Mount), без записи на флеш:
 *   1. Слот 0 каждого сектора: стёрт, годен (seq известен) или испорчен —
 *      тогда ищем годный слот дальше в секторе и выводим seq слота 0 из него.
 *   2. Голова — сектор с наибольшим seq. Позиция записи — за последним
 *      нестёртым слотом головы: испорченный обрывом слот пропускается, он
 *      уже израсходовал свой номер (G2: потеряна одна запись).
 *   3. Хвост — идём от головы назад, пока seq предыдущего сектора ровно на
 *      64 меньше. Так в журнал не попадают ни чужие данные (старый том),
 *      ни запасной сектор, стирание которого могло оборваться.
 *   4. Запасной сектор проверяется на чистоту целиком; не чист — его сотрёт
 *      первая же дозапись.
 *
 * Флаг занятости флеш для MSC и запрет записи во время USB-сессии — забота
 * main.c; ожидание BUSY микросхемы — внутри SPI_Flash_*. Журнал только
 * пинает сторожа (kick) в долгих местах: сканирование, стирание, импорт. */

#include <stdint.h>
#include <string.h>
#include "log_record.h"

#define LOG_SECTOR_SIZE     4096U
#define LOG_SLOTS           (LOG_SECTOR_SIZE / LOG_RECORD_SIZE)   /* 64 */
#define LOG_FLASH_SECTORS   512U          /* SST26VF016B: 2 МБ / 4 КБ */

typedef struct {
    /* 1 — успех, 0 — отказ (ошибка шины, таймаут BUSY). Подписи совпадают с
     * SPI_Flash_Read/Write/EraseSector, так что в прошивке это они и есть. */
    uint8_t (*read)(uint32_t addr, uint8_t *buf, uint32_t len);
    uint8_t (*program)(uint32_t addr, const uint8_t *buf, uint32_t len);
    uint8_t (*erase)(uint32_t addr);      /* сектор 4 КБ, addr выровнен */
    void    (*kick)(void);                /* сторож; может быть NULL */
} LogFlashOps;

typedef struct {
    const LogFlashOps *ops;
    uint32_t nsect;          /* секторов в журнале (на плате — 512) */
    uint32_t w_sect;         /* сектор следующей записи */
    uint32_t w_slot;         /* слот следующей записи, 0..63 */
    uint32_t next_seq;       /* seq следующей записи */
    uint32_t tail_sect;      /* сектор старейшей читаемой записи (её слот 0) */
    uint32_t tail_seq;       /* seq старейшей читаемой записи */
    uint8_t  spare_erased;   /* запасной сектор (за w_sect) заведомо чист */
    uint8_t  w_erased;       /* сектор записи заведомо чист (важно при w_slot 0) */
    uint8_t  mounted;
} LogRing;

/* Итог монтирования. */
#define LOG_MOUNT_OK       0U   /* журнал найден */
#define LOG_MOUNT_EMPTY    1U   /* слоты 0 всех секторов стёрты — журнала нет */
#define LOG_MOUNT_FOREIGN  2U   /* журнала нет, а флеш не чиста (старый том?) */
#define LOG_MOUNT_EIO      3U   /* флеш не читается — ничего не решаем */

/* Чтение. */
#define LOG_READ_OK   0U
#define LOG_READ_BAD  1U   /* слот испорчен (обрыв) — записи нет, номер занят */
#define LOG_READ_EIO  2U
#define LOG_READ_NONE 3U   /* i вне журнала */

uint8_t  LogRing_Mount(LogRing *r, const LogFlashOps *ops, uint32_t nsect);
void     LogRing_Start(LogRing *r, const LogFlashOps *ops, uint32_t nsect,
                       uint32_t first_sect, uint32_t first_seq);
uint8_t  LogRing_Append(LogRing *r, LogRecord *rec);
uint32_t LogRing_Count(const LogRing *r);
uint32_t LogRing_FirstSeq(const LogRing *r);
uint8_t  LogRing_Read(const LogRing *r, uint32_t i, LogRecord *out);
uint8_t  LogRing_Newest(const LogRing *r, LogRecord *out, uint32_t *idx);

/* ------------------------------------------- импорт старого архива (FAT)
 *
 * Прошлые прошивки держали на флеш FAT12/16 (MBR, раздел с LBA 63, том
 * заявлен на 4 МБ, физически 2 МБ) с data.txt и data_N.txt. Перед первым
 * стартом журнала их текст как есть переносится в журнал записями TEXT.
 *
 * Как не потерять архив при обрыве (выбран способ «журнал — в свободной
 * области»): журнал импорта начинается с первого сектора выше всего, чем
 * пользуется старый том, — служебной области и кластеров переносимых
 * файлов. Пока импорт не закончен, журнал вниз не заворачивает (это
 * проверяется до начала), так что старый том цел до записи IMPORT_END.
 * Обрыв посреди — при следующем старте журнал кончается не на IMPORT_END и
 * не на замере: импорт не закончен, журнальная область стирается и импорт
 * идёт заново с нуля. Граница области выводится из того же нетронутого
 * тома, поэтому совпадает при каждом повторе. Только после IMPORT_END
 * журнал, заворачивая по кругу, начинает стирать старый том — его текст к
 * тому времени уже в журнале.
 *
 * Не помещается (архив больше ~0.9 МБ) — переносятся самые новые файлы,
 * что помещаются, о прочих — счётчик в IMPORT_END. Владельцу: снять такой
 * архив по USB до прошивки П5 (отчёт П5а). */
#define LOG_IMPORT_MAX_FILES  16U

typedef struct {
    char     name[LOG_IMPORT_NAME];   /* "data_3.txt" */
    uint32_t clust;                   /* первый кластер */
    uint32_t size;                    /* байт */
    uint32_t order;                   /* старше — меньше */
    uint32_t end;                     /* байт за последним кластером файла */
} LogOldFile;

typedef struct {
    uint32_t vol_lba;        /* загрузочный сектор тома, в секторах по 512 */
    uint32_t fat_lba, root_lba, data_lba;
    uint32_t spc, nclst, fatbits;
    uint32_t meta_end;       /* байт за служебной областью (корень включительно) */
    uint32_t end;            /* байт за последним используемым — выровнен на 4 КБ */
    uint32_t total;          /* байт текста в переносимых файлах */
    uint8_t  nfiles;
    uint8_t  dropped;        /* файлов не влезло или не прочлось */
    LogOldFile files[LOG_IMPORT_MAX_FILES];
} LogOldVolume;

uint8_t LogImport_Probe(const LogFlashOps *ops, uint32_t flash_bytes, LogOldVolume *v);

/* Итог открытия архива при старте. */
#define LOG_OPEN_OK         0U
#define LOG_OPEN_EIO        1U   /* флеш не читается */
#define LOG_OPEN_NEED_POWER 2U   /* нужно писать (импорт), а питание не позволяет */
#define LOG_OPEN_FAIL       3U   /* импорт сорвался на записи — повторится */

uint8_t LogArchive_Open(LogRing *r, const LogFlashOps *ops, uint32_t nsect,
                        uint8_t may_write, LogOldVolume *scratch);

/* ====================================================== реализация */
#ifdef LOG_RING_IMPLEMENTATION

static void LogRing_Kick(const LogFlashOps *ops)
{
    if (ops->kick)
        ops->kick();
}

static uint32_t LogRing_Next(const LogRing *r, uint32_t s)
{
    return (s + 1U == r->nsect) ? 0U : s + 1U;
}

static uint32_t LogRing_Prev(const LogRing *r, uint32_t s)
{
    return (s == 0U) ? r->nsect - 1U : s - 1U;
}

static uint32_t LogRing_Addr(uint32_t sect, uint32_t slot)
{
    return sect * LOG_SECTOR_SIZE + slot * LOG_RECORD_SIZE;
}

/* Чист ли сектор целиком. 0 — нет или не прочитан (тогда сотрём: лишнее
 * стирание дешевле записи по грязному). */
static uint8_t LogRing_SectorBlank(const LogFlashOps *ops, uint32_t sect)
{
    LogRecord buf;
    for (uint32_t k = 0; k < LOG_SLOTS; k++)
    {
        if (!ops->read(LogRing_Addr(sect, k), buf.b, LOG_RECORD_SIZE))
            return 0;
        if (!LogRecord_IsErased(&buf))
            return 0;
    }
    return 1;
}

/* seq слота 0 сектора: 1 — известен; 0 — в секторе нет ни одной годной
 * записи. *any — в секторе есть нестёртые слоты (смотрится только слот 0,
 * если он годен). *eio — сбой чтения. */
static uint8_t LogRing_SectorSeq(const LogFlashOps *ops, uint32_t sect, uint32_t *seq0,
                                 uint8_t *any, uint8_t *eio)
{
    LogRecord buf;
    *any = 0;
    for (uint32_t k = 0; k < LOG_SLOTS; k++)
    {
        if (!ops->read(LogRing_Addr(sect, k), buf.b, LOG_RECORD_SIZE))
        {
            *eio = 1;
            return 0;
        }
        uint8_t st = LogRecord_Check(&buf);
        if (st == LOG_REC_OK)
        {
            *any = 1;
            *seq0 = LogRecord_Seq(&buf) - k;
            return 1;
        }
        if (st == LOG_REC_ERASED)
        {
            if (k == 0U)
                return 0;   /* слот 0 стёрт — сектор пуст: пишется подряд с нуля */
            continue;
        }
        *any = 1;
        /* Слот 0 не похож даже на недописанную запись — сектор чужой (старый
         * том), дальше не читаем: иначе каждый старт читал бы весь чип. */
        if (k == 0U && !LogRecord_MayBeOurs(&buf))
            return 0;
    }
    return 0;
}

void LogRing_Start(LogRing *r, const LogFlashOps *ops, uint32_t nsect,
                   uint32_t first_sect, uint32_t first_seq)
{
    r->ops = ops;
    r->nsect = nsect;
    r->w_sect = first_sect;
    r->w_slot = 0;
    r->next_seq = first_seq;
    r->tail_sect = first_sect;
    r->tail_seq = first_seq;
    r->spare_erased = 0;
    r->w_erased = 0;
    r->mounted = 1;
}

uint8_t LogRing_Mount(LogRing *r, const LogFlashOps *ops, uint32_t nsect)
{
    uint32_t head = 0, head_seq0 = 0;
    uint8_t  have = 0, foreign = 0, eio = 0;

    r->mounted = 0;
    r->ops = ops;
    r->nsect = nsect;

    for (uint32_t s = 0; s < nsect; s++)
    {
        uint32_t seq0;
        uint8_t  any;
        if ((s & 31U) == 0U)
            LogRing_Kick(ops);
        if (LogRing_SectorSeq(ops, s, &seq0, &any, &eio))
        {
            /* Наибольший по модулю 2^32: разность со старым максимумом
             * «положительна». Номер не переполнится и за тысячу лет записи раз
             * в минуту, но сравнение честное. */
            if (!have || (int32_t)(seq0 - head_seq0) > 0)
            {
                head = s;
                head_seq0 = seq0;
                have = 1;
            }
        }
        else if (any)
            foreign = 1;
        if (eio)
            return LOG_MOUNT_EIO;
    }

    if (!have)
    {
        LogRing_Start(r, ops, nsect, 0, 0);
        return foreign ? LOG_MOUNT_FOREIGN : LOG_MOUNT_EMPTY;
    }

    /* Позиция записи — за последним нестёртым слотом головы. */
    LogRecord buf;
    uint32_t k = LOG_SLOTS;
    while (k > 0U)
    {
        if (!ops->read(LogRing_Addr(head, k - 1U), buf.b, LOG_RECORD_SIZE))
            return LOG_MOUNT_EIO;
        if (!LogRecord_IsErased(&buf))
            break;
        k--;
    }

    r->next_seq = head_seq0 + k;
    if (k == LOG_SLOTS)
    {
        r->w_sect = LogRing_Next(r, head);
        r->w_slot = 0;
        /* Голова полна: следующий сектор должен быть чист. Не чист — в нём
         * недописанный слот 0 или прерванное стирание; годных записей там нет
         * (иначе головой был бы он), стереть его ничего не теряет. */
        r->w_erased = LogRing_SectorBlank(ops, r->w_sect);
    }
    else
    {
        r->w_sect = head;
        r->w_slot = k;
        r->w_erased = 1;
    }

    /* Хвост: назад от головы, пока номера идут подряд. Сектор записи (если
     * он пуст) и запасной в журнал не входят никогда. */
    uint32_t spare = LogRing_Next(r, r->w_sect);
    uint32_t t = head, t_seq0 = head_seq0;
    for (uint32_t n = 1; n < nsect; n++)
    {
        uint32_t p = LogRing_Prev(r, t);
        uint32_t seq0;
        uint8_t  any;
        if (p == spare || p == r->w_sect)
            break;
        if ((n & 31U) == 0U)
            LogRing_Kick(ops);
        if (!LogRing_SectorSeq(ops, p, &seq0, &any, &eio))
        {
            if (eio)
                return LOG_MOUNT_EIO;
            break;
        }
        if (seq0 != t_seq0 - LOG_SLOTS)
            break;
        t = p;
        t_seq0 = seq0;
    }
    r->tail_sect = t;
    r->tail_seq = t_seq0;

    r->spare_erased = LogRing_SectorBlank(ops, spare);
    r->mounted = 1;
    LogRing_Kick(ops);
    return LOG_MOUNT_OK;
}

static uint8_t LogRing_Erase(LogRing *r, uint32_t sect)
{
    LogRing_Kick(r->ops);
    uint8_t ok = r->ops->erase(sect * LOG_SECTOR_SIZE);
    LogRing_Kick(r->ops);   /* стирание сектора — до ~400 мс по паспорту */
    return ok;
}

/* Стереть запасной сектор. Если в нём лежат старейшие записи журнала —
 * хвост уходит вперёд на сектор (G1: старейшие пропадают посекторно). */
static uint8_t LogRing_EraseSpare(LogRing *r)
{
    uint32_t spare = LogRing_Next(r, r->w_sect);
    if (r->next_seq != r->tail_seq && r->tail_sect == spare)
    {
        r->tail_sect = LogRing_Next(r, spare);
        r->tail_seq += LOG_SLOTS;
    }
    if (!LogRing_Erase(r, spare))
        return 0;
    r->spare_erased = 1;
    return 1;
}

/* Занять текущий слот: номер расходуется при любом исходе. */
static void LogRing_Advance(LogRing *r)
{
    r->next_seq++;
    if (++r->w_slot == LOG_SLOTS)
    {
        r->w_slot = 0;
        r->w_sect = LogRing_Next(r, r->w_sect);
        /* Новый сектор записи был запасным, а новый запасной — это старейший
         * сектор журнала (или ещё не тронутый). */
        r->w_erased = r->spare_erased;
        r->spare_erased = 0;
    }
}

static uint8_t LogRing_TryAppend(LogRing *r, LogRecord *rec)
{
    if (r->w_slot == 0U && !r->w_erased)
    {
        /* Сектор записи не стёрт: после старта на чужой флеш или после
         * сорванного стирания. Читаемых записей в нём нет (см. Mount). */
        if (!LogRing_Erase(r, r->w_sect))
            return 0;
        r->w_erased = 1;
    }
    if (!r->spare_erased && !LogRing_EraseSpare(r))
    {
        /* Запасной не стёрся — писать в свой сектор ещё можно, кроме
         * последнего слота: за ним запись перешла бы в грязный сектор. */
        if (r->w_slot == LOG_SLOTS - 1U)
            return 0;
    }

    LogRecord_Seal(rec, r->next_seq);
    uint32_t addr = LogRing_Addr(r->w_sect, r->w_slot);
    uint8_t ok = r->ops->program(addr, rec->b, LOG_RECORD_SIZE);
    if (ok)
    {
        /* Программирование на просевшем питании «проходит» без единой
         * ошибки шины — сверяем. */
        LogRecord back;
        ok = r->ops->read(addr, back.b, LOG_RECORD_SIZE) &&
             memcmp(back.b, rec->b, LOG_RECORD_SIZE) == 0;
    }
    LogRing_Advance(r);

    /* Запись перешла в новый сектор — стираем следующий запасной сразу,
     * а не перед следующей записью: «всегда минимум один стёртый впереди». */
    if (r->w_slot == 0U && !r->spare_erased)
        (void)LogRing_EraseSpare(r);
    return ok;
}

/* Дописать запись. Номер проставляется здесь. Не сошлось — одна повторная
 * попытка в следующий слот: испорченный слот уже занят, его номер пропущен,
 * а замер не теряется. Возвращает 1 при успехе. */
uint8_t LogRing_Append(LogRing *r, LogRecord *rec)
{
    if (!r->mounted)
        return 0;
    if (LogRing_TryAppend(r, rec))
        return 1;
    return LogRing_TryAppend(r, rec);
}

uint32_t LogRing_Count(const LogRing *r)
{
    return r->mounted ? r->next_seq - r->tail_seq : 0U;
}

uint32_t LogRing_FirstSeq(const LogRing *r)
{
    return r->tail_seq;
}

/* i-я запись от старейшей: O(1), одно чтение 64 байт. */
uint8_t LogRing_Read(const LogRing *r, uint32_t i, LogRecord *out)
{
    if (i >= LogRing_Count(r))
        return LOG_READ_NONE;
    uint32_t sect = (r->tail_sect + i / LOG_SLOTS) % r->nsect;
    if (!r->ops->read(LogRing_Addr(sect, i % LOG_SLOTS), out->b, LOG_RECORD_SIZE))
        return LOG_READ_EIO;
    /* Номер сверяется: годная CRC при чужом номере — это не та запись. */
    if (LogRecord_Check(out) != LOG_REC_OK || LogRecord_Seq(out) != r->tail_seq + i)
        return LOG_READ_BAD;
    return LOG_READ_OK;
}

/* Самая новая годная запись (испорченные пропускаются). 1 — нашлась. */
uint8_t LogRing_Newest(const LogRing *r, LogRecord *out, uint32_t *idx)
{
    uint32_t n = LogRing_Count(r);
    while (n > 0U)
    {
        n--;
        uint8_t st = LogRing_Read(r, n, out);
        if (st == LOG_READ_EIO)
            return 0;
        if (st == LOG_READ_OK)
        {
            if (idx)
                *idx = n;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------- старый том FAT */

#define LOG_FAT_SECTOR  512U

static uint8_t LogImport_ReadSector(const LogFlashOps *ops, uint32_t flash_bytes,
                                    uint32_t lba, uint8_t *buf)
{
    if (lba >= flash_bytes / LOG_FAT_SECTOR)
        return 0;
    return ops->read(lba * LOG_FAT_SECTOR, buf, LOG_FAT_SECTOR);
}

/* Загрузочный сектор FAT12/16 с 512-байтным сектором? Заполняет геометрию. */
static uint8_t LogImport_Boot(const uint8_t *b, uint32_t lba, LogOldVolume *v)
{
    if (b[510] != 0x55 || b[511] != 0xAA || (b[0] != 0xEB && b[0] != 0xE9))
        return 0;
    uint32_t bps   = LogRecord_Get16(b + 11);
    uint32_t spc   = b[13];
    uint32_t rsvd  = LogRecord_Get16(b + 14);
    uint32_t nfats = b[16];
    uint32_t rootn = LogRecord_Get16(b + 17);
    uint32_t tot   = LogRecord_Get16(b + 19);
    uint32_t fatsz = LogRecord_Get16(b + 22);
    if (tot == 0U)
        tot = LogRecord_Get32(b + 32);
    if (bps != LOG_FAT_SECTOR || spc == 0U || (spc & (spc - 1U)) != 0U || rsvd == 0U ||
        nfats == 0U || nfats > 2U || rootn == 0U || fatsz == 0U)
        return 0;
    uint32_t rootsz = (rootn * 32U + LOG_FAT_SECTOR - 1U) / LOG_FAT_SECTOR;
    uint32_t meta = rsvd + nfats * fatsz + rootsz;
    if (tot <= meta)
        return 0;
    uint32_t nclst = (tot - meta) / spc;
    if (nclst == 0U || nclst >= 0xFFF5U)
        return 0;   /* FAT32 у нас не бывало */
    v->vol_lba  = lba;
    v->fat_lba  = lba + rsvd;
    v->root_lba = lba + rsvd + nfats * fatsz;
    v->data_lba = v->root_lba + rootsz;
    v->spc      = spc;
    v->nclst    = nclst;
    v->fatbits  = (nclst <= 0xFF4U) ? 12U : 16U;
    v->meta_end = v->data_lba * LOG_FAT_SECTOR;
    return 1;
}

/* Следующий кластер по FAT; 0 — ошибка чтения или битая цепочка,
 * 0xFFFFFFFF — конец файла. */
static uint32_t LogImport_NextClust(const LogFlashOps *ops, uint32_t flash_bytes,
                                    const LogOldVolume *v, uint32_t c)
{
    uint32_t ofs = (v->fatbits == 12U) ? c + c / 2U : c * 2U;
    uint8_t  e[2];
    uint32_t addr = v->fat_lba * LOG_FAT_SECTOR + ofs;
    if (addr + 2U > flash_bytes || !ops->read(addr, e, 2))
        return 0;
    uint32_t n = LogRecord_Get16(e);
    if (v->fatbits == 12U)
    {
        n = (c & 1U) ? (n >> 4) : (n & 0xFFFU);
        if (n >= 0xFF8U)
            return 0xFFFFFFFFUL;
    }
    else if (n >= 0xFFF8U)
        return 0xFFFFFFFFUL;
    if (n < 2U || n >= v->nclst + 2U)
        return 0;
    return n;
}

static uint32_t LogImport_ClustAddr(const LogOldVolume *v, uint32_t c)
{
    return (v->data_lba + (c - 2U) * v->spc) * LOG_FAT_SECTOR;
}

/* Пройти цепочку файла: длина хватает на размер, кластеры — на физической
 * флеш, без петель. *last — байт за последним кластером. */
static uint8_t LogImport_CheckChain(const LogFlashOps *ops, uint32_t flash_bytes,
                                    const LogOldVolume *v, const LogOldFile *f, uint32_t *last)
{
    uint32_t csize = v->spc * LOG_FAT_SECTOR;
    uint32_t need  = (f->size + csize - 1U) / csize;
    uint32_t c = f->clust;
    *last = 0;
    for (uint32_t i = 0; i < need; i++)
    {
        if (c < 2U || c >= v->nclst + 2U)
            return 0;
        uint32_t end = LogImport_ClustAddr(v, c) + csize;
        if (end > flash_bytes)
            return 0;
        if (end > *last)
            *last = end;
        if (i + 1U < need)
        {
            c = LogImport_NextClust(ops, flash_bytes, v, c);
            if (c == 0U || c == 0xFFFFFFFFUL)
                return 0;
        }
        if ((i & 255U) == 255U)
            LogRing_Kick(ops);
    }
    return 1;
}

/* Имя 8.3 из записи каталога: "DATA_12 TXT" -> "data_12.txt". Порядок
 * переноса: data.csv (самая старая прошивка), data_N.txt по N, data.txt
 * (текущий) последним. 0 — не наш файл. */
static uint8_t LogImport_Name(const uint8_t *de, char *name, uint32_t *order)
{
    char base[9], ext[4];
    uint32_t n = 0, e = 0;
    for (uint32_t i = 0; i < 8 && de[i] != ' '; i++)
    {
        char ch = (char)de[i];
        base[n++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    base[n] = '\0';
    for (uint32_t i = 8; i < 11 && de[i] != ' '; i++)
    {
        char ch = (char)de[i];
        ext[e++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    ext[e] = '\0';

    if (strcmp(base, "data") == 0 && strcmp(ext, "csv") == 0)
        *order = 0U;
    else if (strcmp(base, "data") == 0 && strcmp(ext, "txt") == 0)
        *order = 0xFFFFFFFFUL;
    else if (strncmp(base, "data_", 5) == 0 && n > 5U && n <= 8U && strcmp(ext, "txt") == 0)
    {
        uint32_t num = LogRecord_Digits(base + 5, n - 5U);
        if (num == 0U || num > 999U)
            return 0;
        *order = num;
    }
    else
        return 0;
    snprintf(name, LOG_IMPORT_NAME, "%s.%s", base, ext);
    return 1;
}

static void LogImport_AddFile(LogOldVolume *v, const LogOldFile *f)
{
    /* Список упорядочен по возрастанию order; переполнен — выпадает самый
     * старый (он и так ушёл бы первым при нехватке места). */
    uint32_t i = v->nfiles;
    if (i == LOG_IMPORT_MAX_FILES)
    {
        v->dropped++;
        if (f->order < v->files[0].order)
            return;
        memmove(&v->files[0], &v->files[1], (LOG_IMPORT_MAX_FILES - 1U) * sizeof(LogOldFile));
        i = LOG_IMPORT_MAX_FILES - 1U;
    }
    else
        v->nfiles++;
    while (i > 0U && v->files[i - 1U].order > f->order)
    {
        v->files[i] = v->files[i - 1U];
        i--;
    }
    v->files[i] = *f;
}

uint8_t LogImport_Probe(const LogFlashOps *ops, uint32_t flash_bytes, LogOldVolume *v)
{
    uint8_t sec[LOG_FAT_SECTOR];
    memset(v, 0, sizeof(*v));

    /* Где том: по MBR, по LBA 63 (наш f_mkfs; MBR мог быть стёрт прерванной
     * правкой — R3) или без MBR с нуля. */
    uint32_t cand[3] = { 0xFFFFFFFFUL, 63U, 0U };
    if (LogImport_ReadSector(ops, flash_bytes, 0, sec) && sec[510] == 0x55 && sec[511] == 0xAA)
        cand[0] = LogRecord_Get32(sec + 446 + 8);
    uint8_t found = 0;
    for (uint32_t i = 0; i < 3 && !found; i++)
    {
        if (cand[i] == 0xFFFFFFFFUL || !LogImport_ReadSector(ops, flash_bytes, cand[i], sec))
            continue;
        found = LogImport_Boot(sec, cand[i], v);
    }
    if (!found || v->meta_end > flash_bytes)
        return 0;

    /* Корневой каталог: только наши файлы, удалённые и каталоги — мимо. */
    uint32_t last = v->meta_end;
    uint8_t  done = 0;
    for (uint32_t lba = v->root_lba; lba < v->data_lba && !done; lba++)
    {
        if (!LogImport_ReadSector(ops, flash_bytes, lba, sec))
            return 0;
        for (uint32_t o = 0; o < LOG_FAT_SECTOR; o += 32U)
        {
            const uint8_t *de = sec + o;
            if (de[0] == 0x00) { done = 1; break; }
            if (de[0] == 0xE5 || (de[11] & 0x18U) != 0U || de[11] == 0x0FU)
                continue;
            LogOldFile f;
            memset(&f, 0, sizeof(f));
            if (!LogImport_Name(de, f.name, &f.order))
                continue;
            f.clust = LogRecord_Get16(de + 26);
            f.size  = LogRecord_Get32(de + 28);
            if (f.size == 0U)
                continue;
            if (!LogImport_CheckChain(ops, flash_bytes, v, &f, &f.end))
            {
                v->dropped++;   /* битая цепочка — не наш случай гадать */
                continue;
            }
            LogImport_AddFile(v, &f);
        }
    }

    /* Граница — по всем оставшимся файлам. */
    v->total = 0;
    for (uint32_t i = 0; i < v->nfiles; i++)
    {
        if (v->files[i].end > last)
            last = v->files[i].end;
        v->total += v->files[i].size;
    }
    v->end = (last + LOG_SECTOR_SIZE - 1U) / LOG_SECTOR_SIZE * LOG_SECTOR_SIZE;
    return 1;
}

/* Сколько секторов журнала займёт импорт: FILE на файл, TEXT по 52 байта,
 * END; плюс запасной сектор за последним. */
static uint32_t LogImport_Sectors(const LogOldVolume *v)
{
    uint32_t recs = 1U;
    for (uint32_t i = 0; i < v->nfiles; i++)
        recs += 1U + (v->files[i].size + LOG_TEXT_MAX - 1U) / LOG_TEXT_MAX;
    /* Запас на повторы испорченных слотов — по одному на сектор. */
    recs += recs / LOG_SLOTS + 1U;
    return (recs + LOG_SLOTS - 1U) / LOG_SLOTS + 1U;
}

/* Переложить тексты в журнал. Журнал уже начат с v->end. */
static uint8_t LogImport_Run(LogRing *r, const LogOldVolume *v, uint32_t flash_bytes)
{
    LogRecord rec;
    uint8_t   chunk[LOG_TEXT_MAX];
    uint32_t  csize = v->spc * LOG_FAT_SECTOR;
    uint32_t  total = 0;

    for (uint32_t i = 0; i < v->nfiles; i++)
    {
        const LogOldFile *f = &v->files[i];
        LogRecord_MakeImportFile(&rec, (uint8_t)i, f->size, f->name);
        if (!LogRing_Append(r, &rec))
            return 0;

        uint32_t c = f->clust, in_clust = 0, left = f->size;
        while (left > 0U)
        {
            uint32_t n = 0;
            while (n < LOG_TEXT_MAX && left > 0U)
            {
                if (in_clust == csize)
                {
                    c = LogImport_NextClust(r->ops, flash_bytes, v, c);
                    if (c == 0U || c == 0xFFFFFFFFUL)
                        return 0;
                    in_clust = 0;
                }
                uint32_t part = csize - in_clust;
                if (part > LOG_TEXT_MAX - n) part = LOG_TEXT_MAX - n;
                if (part > left) part = left;
                if (!r->ops->read(LogImport_ClustAddr(v, c) + in_clust, chunk + n, part))
                    return 0;
                n += part;
                in_clust += part;
                left -= part;
            }
            LogRecord_MakeText(&rec, (uint8_t)i, chunk, n);
            if (!LogRing_Append(r, &rec))
                return 0;
            total += n;
            LogRing_Kick(r->ops);
        }
    }
    LogRecord_MakeImportEnd(&rec, v->nfiles, total);
    rec.b[LOG_PAYLOAD_OFS + 5] = v->dropped;
    return LogRing_Append(r, &rec);
}

/* Импорт начат и не закончен: самая новая годная запись — FILE или TEXT. */
static uint8_t LogImport_Pending(const LogRing *r)
{
    LogRecord rec;
    if (!LogRing_Newest(r, &rec, NULL))
        return 0;
    return LogRecord_Type(&rec) == LOG_TYPE_IMPORT_FILE || LogRecord_Type(&rec) == LOG_TYPE_TEXT;
}

uint8_t LogArchive_Open(LogRing *r, const LogFlashOps *ops, uint32_t nsect,
                        uint8_t may_write, LogOldVolume *v)
{
    uint32_t flash_bytes = nsect * LOG_SECTOR_SIZE;
    uint8_t  m = LogRing_Mount(r, ops, nsect);

    if (m == LOG_MOUNT_EIO)
        return LOG_OPEN_EIO;
    if (m == LOG_MOUNT_EMPTY)
        return LOG_OPEN_OK;          /* чистая флеш: журнал с сектора 0 */

    uint8_t pending = (m == LOG_MOUNT_OK) && LogImport_Pending(r);
    if (m == LOG_MOUNT_OK && !pending)
        return LOG_OPEN_OK;

    /* Дальше — запись: импорт или его перезапуск. */
    r->mounted = 0;
    if (!may_write)
        return LOG_OPEN_NEED_POWER;

    uint8_t vol = LogImport_Probe(ops, flash_bytes, v);
    if (pending)
    {
        if (!vol)
        {
            /* Старого тома больше не видно, хотя импорт не закончен. Ниже
             * границы журнал не писал — значит, том испортил кто-то другой.
             * Повторять нечего: закрываем импорт тем, что успели. */
            (void)LogRing_Mount(r, ops, nsect);
            LogRecord rec;
            LogRecord_MakeImportEnd(&rec, 0, 0);
            return LogRing_Append(r, &rec) ? LOG_OPEN_OK : LOG_OPEN_FAIL;
        }
        /* Стереть журнальную область целиком и начать заново. Только выше
         * границы тома — сам том не трогаем. Обрыв здесь — опять «не
         * закончен» (или «журнала нет»), и всё повторится. */
        for (uint32_t s = v->end / LOG_SECTOR_SIZE; s < nsect; s++)
        {
            LogRing_Kick(ops);
            if (LogRing_SectorBlank(ops, s))
                continue;
            if (!ops->erase(s * LOG_SECTOR_SIZE))
                return LOG_OPEN_FAIL;
        }
    }

    if (!vol || v->nfiles == 0U)
    {
        /* Не наш том или переносить нечего: журнал с начала, чужое
         * стирается по мере хода кольца. */
        LogRing_Start(r, ops, nsect, 0, 0);
        return LOG_OPEN_OK;
    }

    /* Не помещается выше тома — без самых старых файлов. */
    uint32_t first = v->end / LOG_SECTOR_SIZE;
    while (v->nfiles > 0U && first + LogImport_Sectors(v) > nsect)
    {
        v->total -= v->files[0].size;
        memmove(&v->files[0], &v->files[1], (v->nfiles - 1U) * sizeof(LogOldFile));
        v->nfiles--;
        v->dropped++;
    }
    if (v->nfiles == 0U || first >= nsect)
    {
        LogRing_Start(r, ops, nsect, 0, 0);
        return LOG_OPEN_OK;
    }

    LogRing_Start(r, ops, nsect, first, 0);
    return LogImport_Run(r, v, flash_bytes) ? LOG_OPEN_OK : LOG_OPEN_FAIL;
}

#endif /* LOG_RING_IMPLEMENTATION */
#endif
