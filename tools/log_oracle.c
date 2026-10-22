/* Эталон для tools/test_tools.py: журнал прошивки на модели флеш и файлы,
 * которые из него отдаёт том USB.
 *
 *   gcc -std=c11 -ICore/Inc -o log_oracle tools/log_oracle.c -lm
 *   log_oracle <сценарий> <папка>
 *
 * Записи кладёт настоящий LogRing_Append (log_ring.h), файлы собирает
 * настоящий VFat (vfat.h) и читаются они секторами тома, как их видит хост.
 * На выходе — <сценарий>.img (сырой образ флеш, как его снимает
 * read_data.sh --swd), <сценарий>.data.txt / .old.txt / .info.txt (что
 * лежит на томе) и строка сведений в stdout. tools/log_image.py должен из
 * образа получить те же файлы байт в байт.
 *
 * Не прошивка и не хостовый тест прошивки: только эталон для инструментов. */
#define LOG_RING_IMPLEMENTATION
#include "log_ring.h"
#define VFAT_IMPLEMENTATION
/* vfat.h берёт серийный номер тома из уникального ID МК (П5б+, V3). У оракула
 * МК нет, а серийный номер в сверяемые файлы не попадает — хватит нулей. */
#define VFAT_DEVICE_UID(w) ((w)[0] = 0U, (w)[1] = 0U, (w)[2] = 0U)
#include "vfat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NSECT  LOG_FLASH_SECTORS
static uint8_t fl[NSECT * LOG_SECTOR_SIZE];

static uint8_t f_read(uint32_t a, uint8_t *b, uint32_t n)
{
    if (a + n > sizeof(fl)) return 0;
    memcpy(b, fl + a, n);
    return 1;
}

/* NOR: программирование только сбрасывает биты. */
static uint8_t f_prog(uint32_t a, const uint8_t *b, uint32_t n)
{
    if (a + n > sizeof(fl)) return 0;
    for (uint32_t i = 0; i < n; i++) fl[a + i] &= b[i];
    return 1;
}

static uint8_t f_erase(uint32_t a)
{
    if (a % LOG_SECTOR_SIZE || a >= sizeof(fl)) return 0;
    memset(fl + a, 0xFF, LOG_SECTOR_SIZE);
    return 1;
}

static const LogFlashOps ops = { f_read, f_prog, f_erase, NULL, NULL };

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static float rf(void)
{
    uint32_t k = rnd() % 16U;
    float f;
    switch (k)
    {
    case 0: return NAN;
    case 1: return -NAN;
    case 2: return INFINITY;
    case 3: return -INFINITY;
    case 4: return -0.0f;
    case 5: { uint32_t b = rnd(); memcpy(&f, &b, 4); return f; }      /* любой образ */
    case 6: { uint32_t b = rnd() & 0x807FFFFFU; memcpy(&f, &b, 4); return f; } /* субнормальные */
    case 7: return (float)(rnd() % 2000U) / 8.0f - 100.0f;              /* точные половинки */
    default: return (float)((int32_t)(rnd() % 20000U) - 5000) / 97.0f;
    }
}

static float rraw(void)
{
    uint32_t k = rnd() % 8U;
    if (k == 0) return NAN;
    if (k == 1) return -5.0f;
    if (k == 2) return 70000.0f;
    return (float)(rnd() % 4096U) + ((k == 3) ? 0.5f : 0.0f);
}

static void make_meas(LogRecord *r, uint32_t n)
{
    LogMeas m;
    memset(&m, 0, sizeof(m));
    if (rnd() % 23U == 0U)
        strcpy(m.ts, "2026-1x-01 00:00:00");   /* неразобранная метка */
    else
        snprintf(m.ts, sizeof(m.ts), "%04u-%02u-%02u %02u:%02u:%02u",
                 2000U + rnd() % 255U, rnd() % 100U, rnd() % 100U,
                 rnd() % 100U, rnd() % 100U, (unsigned)(n % 60U));
    m.air_temp = rf(); m.air_hum = rf(); m.soil_temp = rf(); m.soil_hum = rf();
    m.bat_valid = (uint8_t)(rnd() % 4U != 0U);
    m.bat_low = (uint8_t)(rnd() % 3U == 0U);
    m.bat_percent = (uint8_t)rnd();
    m.bat_volts = rf(); m.vdd = rf();
    m.lse_ok = (uint8_t)(rnd() & 1U); m.time_ok = (uint8_t)(rnd() & 1U);
    m.aht_init_st = (int)(rnd() % 6U) - 1;
    m.aht_read_st = (int)(rnd() % 6U) - 1;
    m.ds_err = (uint8_t)(rnd() % 20U);
    m.air_xchk = (uint8_t)(rnd() % 6U);
    m.i2c_rec = (uint8_t)(rnd() % 6U);
    m.sens_off = rnd() % 70000U;
    m.write_fails = rnd(); m.lowpower_skips = rnd() % 1000U;
    m.reset_cause = (uint16_t)rnd();
    m.boot_count = n;
    m.soil_raw = rraw(); m.bat_raw = rraw();
    LogRecord_PackMeas(r, &m);
}

static void append_meas(LogRing *r, uint32_t count)
{
    static uint32_t n = 0;
    LogRecord rec;
    for (uint32_t i = 0; i < count; i++)
    {
        make_meas(&rec, n++);
        if (!LogRing_Append(r, &rec)) { fprintf(stderr, "append failed\n"); exit(2); }
    }
}

static void append_import(LogRing *r, uint32_t bytes, uint8_t finish)
{
    LogRecord rec;
    uint8_t text[LOG_TEXT_MAX];
    LogRecord_MakeImportFile(&rec, 0, bytes, "data_1.txt");
    if (!LogRing_Append(r, &rec)) exit(2);
    for (uint32_t done = 0; done < bytes;)
    {
        uint32_t k = bytes - done;
        if (k > LOG_TEXT_MAX) k = LOG_TEXT_MAX;
        if (rnd() % 5U == 0U && k > 7U) k = 7U;   /* короткие куски тоже бывают */
        for (uint32_t i = 0; i < k; i++)
            text[i] = (uint8_t)((done + i) % 37U == 36U ? '\n' : ((done + i) % 37U == 35U ? '\r'
                                : 'a' + (done + i) % 26U));
        LogRecord_MakeText(&rec, 0, text, k);
        if (!LogRing_Append(r, &rec)) exit(2);
        done += k;
    }
    if (finish)
    {
        LogRecord_MakeImportEnd(&rec, 1, bytes);
        rec.b[LOG_PAYLOAD_OFS + 5] = 2;
        if (!LogRing_Append(r, &rec)) exit(2);
    }
}

static void put_file(const char *dir, const char *name, const char *suf, const uint8_t *b, size_t n)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s%s", dir, name, suf);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(b, 1, n, f);
    fclose(f);
}

static VFat vol;
static uint8_t file_buf[VFAT_CLUSTERS * VFAT_CLUSTER / 2];

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "log_oracle <сценарий> <папка>\n"); return 2; }
    const char *sc = argv[1], *dir = argv[2];
    LogRing r;
    LogOldVolume old;

    memset(fl, 0xFF, sizeof(fl));
    for (const char *p = sc; *p; p++) rng = rng * 31U + (uint8_t)*p;

    if (strcmp(sc, "foreign") == 0)
    {
        /* Флеш с чужим текстом (старый том без узнаваемого MBR). */
        for (uint32_t i = 0; i < 300000U; i++)
            fl[i] = (uint8_t)("2026-01-01 00:00:00,20.00,40.00\r\n"[i % 34U]);
    }
    else if (strcmp(sc, "empty") != 0)
    {
        if (LogArchive_Open(&r, &ops, NSECT, 1, &old) != LOG_OPEN_OK) return 2;
        if (strcmp(sc, "few") == 0)
            append_meas(&r, 150);
        else if (strcmp(sc, "rings") == 0)
            append_meas(&r, 2U * NSECT * LOG_SLOTS + 1234U);
        else if (strcmp(sc, "seqwrap") == 0)
        {
            /* Номера переходят через 2^32 посреди журнала. */
            LogRing_Start(&r, &ops, NSECT, 500, 0xFFFFFFFFU - 3000U);
            append_meas(&r, 9000);
        }
        else if (strcmp(sc, "damaged") == 0)
        {
            append_meas(&r, 3000);
            /* Бит в нагрузке посреди журнала: слот испорчен, номер занят. */
            fl[20 * LOG_SECTOR_SIZE + 5 * LOG_RECORD_SIZE + 20] ^= 0x10U;
            /* Сектор нулей посреди журнала (мост через него). */
            memset(fl + 30 * LOG_SECTOR_SIZE, 0, LOG_SECTOR_SIZE);
            /* Годная запись не на своём месте (чужой номер): чтение её не
             * выдаёт — сверяется номер слота. */
            memcpy(fl + 25 * LOG_SECTOR_SIZE + 9 * LOG_RECORD_SIZE,
                   fl + 15 * LOG_SECTOR_SIZE + 7 * LOG_RECORD_SIZE, LOG_RECORD_SIZE);
            /* Сброшенный бит в байте типа слота 0 (T3). */
            fl[10 * LOG_SECTOR_SIZE] &= 0xFBU;
            /* Перезапуск посреди сектора: первая дозапись пропустит слот (T4). */
            if (LogRing_Mount(&r, &ops, NSECT) != LOG_MOUNT_OK) return 2;
            append_meas(&r, 100);
            if (LogRing_Mount(&r, &ops, NSECT) != LOG_MOUNT_OK) return 2;
            append_meas(&r, 7);
            /* Недописанная последняя запись (обрыв посреди программирования). */
            {
                LogRecord rec;
                uint32_t a = r.w_sect * LOG_SECTOR_SIZE + r.w_slot * LOG_RECORD_SIZE;
                make_meas(&rec, 0);
                LogRecord_Seal(&rec, r.next_seq);
                f_prog(a, rec.b, 30);
            }
        }
        else if (strcmp(sc, "restart") == 0)
        {
            /* Журнал начат заново вплотную за старыми записями с другими
             * номерами (так бывает после повтора импорта): хвост не должен
             * пристегнуть старые секторы — номера не смежны. */
            append_meas(&r, 200);
            LogRing_Start(&r, &ops, NSECT, 4, 100000U);
            append_meas(&r, 300);
        }
        else if (strcmp(sc, "torn_erase") == 0)
        {
            /* Полный круг, затем оборванное стирание запасного: половина
             * сектора — 0xFF, половина — старые записи. */
            append_meas(&r, NSECT * LOG_SLOTS + 70U);
            if (LogRing_Mount(&r, &ops, NSECT) != LOG_MOUNT_OK) return 2;
            uint32_t spare = (r.w_sect + 1U) % NSECT;
            memset(fl + spare * LOG_SECTOR_SIZE, 0xFF, LOG_SECTOR_SIZE / 2U);
        }
        else if (strcmp(sc, "import") == 0)
        {
            append_import(&r, 9000, 1);
            append_meas(&r, 500);
        }
        else if (strcmp(sc, "import_pending") == 0)
            append_import(&r, 3000, 0);
        else
        {
            fprintf(stderr, "нет сценария %s\n", sc);
            return 2;
        }
    }

    /* Как при подключении USB: журнал открывается только чтением. */
    uint8_t open_st = LogArchive_Open(&r, &ops, NSECT, 0, &old);
    /* Содержимое — и при незаконченном импорте (том его тогда не отдаёт):
     * разбор образа его всё равно показывает. */
    uint8_t mnt = LogRing_Mount(&r, &ops, NSECT);
    VFat_Build(&vol, (mnt == LOG_MOUNT_OK || mnt == LOG_MOUNT_EMPTY) ? &r : NULL, "oracle");

    put_file(dir, sc, ".img", fl, sizeof(fl));
    static const char *suf[VFAT_FILES] = { ".data.txt", ".old.txt", ".info.txt" };
    for (uint32_t f = 0; f < VFAT_FILES; f++)
    {
        if (!vol.present[f])
            continue;
        uint32_t lba = VFAT_PART_LBA + VFAT_DATA + (uint32_t)(vol.clus[f] - 2U) * VFAT_SPC;
        uint32_t ns = (vol.size[f] + VFAT_SECTOR - 1U) / VFAT_SECTOR;
        if (ns * VFAT_SECTOR > sizeof(file_buf)) return 2;
        for (uint32_t k = 0; k < ns; k++)
            if (!VFat_Read(&vol, lba + k, file_buf + k * VFAT_SECTOR)) { fprintf(stderr, "VFat_Read\n"); return 2; }
        put_file(dir, sc, suf[f], file_buf, vol.size[f]);
    }
    printf("%s mount=%u open=%u count=%lu first=%lu meas=%lu bad=%lu vfat=%u\n", sc, mnt, open_st,
           (unsigned long)LogRing_Count(&r), (unsigned long)LogRing_FirstSeq(&r),
           (unsigned long)vol.n_meas, (unsigned long)(vol.n_bad + vol.n_skip), vol.state);
    return 0;
}
