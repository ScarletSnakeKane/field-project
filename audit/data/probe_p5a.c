#define main orig_main
#include "tests/test_main.c"
#undef main

/* read wrapper: fails once on exact (addr,len) */
static long pf_addr = -1; static uint32_t pf_len = 0; static int pf_hits = 0;
static uint8_t pr_read(uint32_t a, uint8_t *b, uint32_t n)
{
    if ((long)a == pf_addr && n == pf_len && pf_hits == 0) { pf_hits++; return 0; }
    return fm_read(a, b, n);
}
static int lock_on = 0;
static uint8_t pr_prog(uint32_t a, const uint8_t *b, uint32_t n) { if (lock_on) return 1; return fm_program(a, b, n); }
static uint8_t pr_erase(uint32_t a) { if (lock_on) return 1; return fm_erase(a); }
static const LogFlashOps PR = { pr_read, pr_prog, pr_erase, NULL };

static void count_types(const LogRing *r, const char *tag)
{
    uint32_t n = LogRing_Count(r), c[256] = {0}, bad = 0;
    LogRecord rec;
    for (uint32_t i = 0; i < n; i++) { if (LogRing_Read(r, i, &rec) == LOG_READ_OK) c[rec.b[0]]++; else bad++; }
    printf("  %s: count=%u F=%u T=%u E=%u M=%u bad=%u tail_sect=%u\n", tag, n, c['F'], c['T'], c['E'], c['M'], bad, r->tail_sect);
}

int main(void)
{
    LogRing r; LogOldVolume v; LogRecord rec;
    static const OldFileSpec F[3] = { { "DATA_1  TXT", 1100, 3 }, { "DATA_2  TXT", 5000, 2 }, { "DATA    TXT", 23456, 1 } };

    for (int which = 0; which < 3; which++) {
        fm_reset(LOG_FLASH_SECTORS); fm_mode = 1;
        uint32_t end = old_volume(16, F, 3, 0, old_texts);
        static uint8_t before[64 * 4096]; memcpy(before, fm, sizeof(before));
        pf_hits = 0;
        if (which == 0) { pf_addr = 63 * 512; pf_len = 512; }          /* boot sector */
        if (which == 1) { pf_addr = 96 * 512; pf_len = 512; }          /* root dir (FAT16: root_lba 96) */
        if (which == 2) {                                              /* FAT entry of data.txt first cluster */
            uint32_t fat = 64 * 512; uint32_t c = fm[96*512 + 2*32 + 32*2 + 26] | (fm[96*512+2*32+32*2+27] << 8);
            pf_addr = fat + c * 2; pf_len = 2; printf("  data.txt first clust %u\n", c);
        }
        uint8_t res = LogArchive_Open(&r, &PR, LOG_FLASH_SECTORS, 1, &v);
        printf("case %d: open=%u hits=%d mounted=%u w_sect=%u tail=%u end=%u\n", which, res, pf_hits, r.mounted, r.w_sect, r.tail_sect, end);
        count_types(&r, "after first open");
        for (uint32_t i = 1; i <= 200; i++) { log_make(&rec, i); LogRing_Append(&r, &rec); }
        res = LogArchive_Open(&r, &PR, LOG_FLASH_SECTORS, 1, &v);
        count_types(&r, "reboot, 200 meas");
        printf("  old volume [0,end) intact: %s; MBR sig %02x%02x\n", memcmp(before, fm, end) == 0 ? "yes" : "NO", fm[510], fm[511]);
        printf("  import ever? Probe now: %u nfiles=%u\n", LogImport_Probe(&FM_OPS, LOG_FLASH_SECTORS*4096U, &v), v.nfiles);
    }

    /* pending re-import + Probe read failure */
    {
        fm_reset(LOG_FLASH_SECTORS); fm_mode = 1;
        old_volume(16, F, 3, 0, old_texts);
        pf_addr = -1;
        fm_ops = 0; fm_cut = 200;
        (void)LogArchive_Open(&r, &PR, LOG_FLASH_SECTORS, 1, &v);
        fm_power_on();
        count_types(&r, "pending state (mount)");
        pf_addr = 96 * 512; pf_len = 512; pf_hits = 0;
        uint8_t res = LogArchive_Open(&r, &PR, LOG_FLASH_SECTORS, 1, &v);
        printf("pending+EIO: open=%u hits=%d\n", res, pf_hits);
        count_types(&r, "after");
        pf_addr = -1;
        res = LogArchive_Open(&r, &PR, LOG_FLASH_SECTORS, 1, &v);
        count_types(&r, "next boot (no fault)");
    }

    /* bit flip 1->0 in byte 0 of slot 0 */
    for (int where = 0; where < 2; where++) {
        fm_reset(64); fm_mode = 1;
        LogRing_Mount(&r, &FM_OPS, 64);
        uint32_t N = 40 * 64 + 30;
        for (uint32_t i = 1; i <= N; i++) { log_make(&rec, i); LogRing_Append(&r, &rec); }
        count_types(&r, where ? "before (head flip)" : "before (middle flip)");
        uint32_t s = where ? r.w_sect : 20;
        fm[s * 4096] &= (uint8_t)~0x01;    /* 'M' 0x4D -> 0x4C */
        LogRing m; LogRing_Mount(&m, &FM_OPS, 64);
        count_types(&m, "after 1-bit flip, mount");
        log_make(&rec, 99999); LogRing_Append(&m, &rec);
        LogRing_Mount(&m, &FM_OPS, 64);
        count_types(&m, "after one more append + mount");
    }

    /* SST26 block protection relocked (flash POR): program/erase silently ignored */
    {
        fm_reset(64); fm_mode = 1; lock_on = 0; pf_addr = -1;
        LogRing_Mount(&r, &PR, 64);
        for (uint32_t i = 1; i <= 100; i++) { log_make(&rec, i); LogRing_Append(&r, &rec); }
        lock_on = 1;
        int fs_ok = 1; uint32_t okc = 0, fails = 0;
        for (uint32_t cyc = 0; cyc < 500; cyc++) {
            if (!fs_ok) fs_ok = (LogArchive_Open(&r, &PR, 64, 1, &v) == LOG_OPEN_OK);
            log_make(&rec, 1000 + cyc);
            if (fs_ok && LogRing_Append(&r, &rec)) okc++; else { fails++; fs_ok = 0; }
        }
        printf("locked flash: 500 cycles -> ok=%u fails=%u (no SPI_Flash_Init in loop)\n", okc, fails);
        lock_on = 0;
        LogRing_Mount(&r, &PR, 64); count_types(&r, "after unlock+mount");
    }
    return 0;
}
