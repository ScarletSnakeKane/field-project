"""Независимая проверка виртуального тома (П5б, гарантия G5).

    python3 -I tests/test_vfat_image.py

Хостовый C-тест (tests/test_main.c, VFAT_ONLY=1) собирает образы тома по
журналу на модели флеш — пустой, одна запись, частичный, с испорченным
слотом, с импортом, после нескольких кругов, частично ушедший импорт, без
журнала — и рядом кладёт ожидаемые data.txt / data_old.txt, собранные
LogRecord_FormatCsv прямо из записей. Здесь образы читают чужие реализации
FAT: fsck.vfat -n (dosfstools), mtools и pyfatfs — какие есть. Каждая
доступная должна принять том и выдать файлы байт в байт. Нет ни одной (или
нет gcc — Windows у владельца) — тест пропускается с сообщением.

Второй тест — под qemu с newlib-nano (если есть arm-none-eabi-gcc и
qemu-system-arm): быстрый форматтер vfat.h сверяется с snprintf прошивочной
libc, и считаются инструкции Cortex-M4 на строку и на сектор 512 Б.

Третий — класс MSC ST на хосте с заглушками LL: отказ чтения посреди READ10
должен дать STALL IN и CSW FAILED после CLEAR_FEATURE (V1, П5б+).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PART_OFS = 63 * 512
SCENARIOS = ["empty", "one", "partial", "damaged", "import", "rings", "import_partial", "no_archive"]
SOURCES = ["tests/test_main.c", "Core/Src/aht20_decode.c", "Core/Src/ds18b20_decode.c",
           "Core/Src/battery_curve.c", "Core/Src/soil_curve.c", "Core/Src/power_policy.c"]


def have(tool):
    return shutil.which(tool) is not None


def pyfatfs_ok():
    try:
        import pyfatfs.PyFatFS  # noqa: F401  (тянет fs и pkg_resources)
        return True
    except Exception:
        return False


class Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.readers = [r for r, ok in (("fsck.vfat", have("fsck.vfat")),
                                       ("mtools", have("mcopy") and have("mdir")),
                                       ("pyfatfs", pyfatfs_ok())) if ok]
        if not have("gcc"):
            raise unittest.SkipTest("нет gcc — образы не собрать (Windows: проверить на Linux/WSL)")
        if not cls.readers:
            raise unittest.SkipTest("нет ни fsck.vfat, ни mtools, ни pyfatfs: "
                                    "apt-get install -y dosfstools mtools или pip install pyfatfs")
        cls.tmp = tempfile.mkdtemp(prefix="vfat_")
        exe = os.path.join(cls.tmp, "t")
        subprocess.run(["gcc", "-std=c11", "-O1", "-ICore/Inc", "-o", exe] + SOURCES + ["-lm"],
                       cwd=ROOT, check=True)
        env = dict(os.environ, VFAT_ONLY="1", VFAT_DUMP_DIR=cls.tmp)
        # Итог C-теста — отдельной проверкой: образы читатели проверяют и
        # тогда, когда он упал, — чтобы видеть, что ловят они сами.
        cls.c_run = subprocess.run([exe], cwd=ROOT, env=env, capture_output=True, text=True)
        print("\n  читатели FAT: " + ", ".join(cls.readers), file=sys.stderr)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def files(self, name):
        want = {}
        for fn, suf in (("data.txt", ".data.txt"), ("data_old.txt", ".old.txt")):
            p = os.path.join(self.tmp, name + suf)
            if os.path.exists(p):
                with open(p, "rb") as f:
                    want[fn] = f.read()
        return want

    def partition(self, name):
        img = os.path.join(self.tmp, name + ".img")
        part = os.path.join(self.tmp, name + ".part")
        with open(img, "rb") as f, open(part, "wb") as g:
            f.seek(PART_OFS)
            g.write(f.read())
        return img, part

    def check_fsck(self, name, part):
        r = subprocess.run(["fsck.vfat", "-n", "-v", part], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, name + ": fsck.vfat -n:\n" + r.stdout + r.stderr)
        self.assertIn("16 bit entries", r.stdout, name + ": не FAT16")
        for bad in ("differ", "Dirty", "corrupt", "Free cluster summary wrong", "Reclaim"):
            self.assertNotIn(bad, r.stdout + r.stderr, name)

    def check_mtools(self, name, img, want):
        drive = img + "@@" + str(PART_OFS)
        env = dict(os.environ, MTOOLS_LOWER_CASE="0")
        r = subprocess.run(["mdir", "-i", drive, "::"], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, name + ": mdir:\n" + r.stderr)
        self.assertIn("FIELDSENSOR", r.stdout, name + ": метка тома")
        listing = r.stdout.lower()
        for fn in ("data.txt", "data_old.txt"):
            short = fn.split(".")[0]
            if fn in want:
                out = os.path.join(self.tmp, name + ".m." + fn)
                c = subprocess.run(["mcopy", "-n", "-i", drive, "::" + fn, out],
                                   capture_output=True, text=True, env=env)
                self.assertEqual(c.returncode, 0, name + ": mcopy " + fn + ": " + c.stderr)
                with open(out, "rb") as f:
                    self.assertEqual(f.read(), want[fn], name + ": " + fn + " (mtools) байт в байт")
            else:
                self.assertNotRegex(listing, r"^" + short + r"\s+txt", name + ": лишний " + fn)
        self.assertRegex(listing, r"(?m)^info\s+txt", name + ": info.txt")

    def check_pyfatfs(self, name, part, want):
        from pyfatfs.PyFat import PyFat
        from pyfatfs.PyFatFS import PyFatFS
        fs = PyFatFS(part, read_only=True)
        try:
            self.assertEqual(fs.fs.fat_type, PyFat.FAT_TYPE_FAT16, name + ": pyfatfs — не FAT16")
            names = {n.lower() for n in fs.listdir("/")}
            for fn in ("data.txt", "data_old.txt"):
                if fn in want:
                    with fs.openbin("/" + fn.upper()) as f:
                        self.assertEqual(f.read(), want[fn], name + ": " + fn + " (pyfatfs) байт в байт")
                else:
                    self.assertNotIn(fn, names, name)
            self.assertIn("info.txt", names, name)
        finally:
            fs.close()

    def test_c_side(self):
        self.assertEqual(self.c_run.returncode, 0, "C-тест тома упал:\n" + self.c_run.stdout[-2000:])

    def test_images(self):
        for name in SCENARIOS:
            with self.subTest(scenario=name):
                img, part = self.partition(name)
                want = self.files(name)
                if name != "no_archive":
                    self.assertIn("data.txt", want, name + ": нет эталона")
                if "fsck.vfat" in self.readers:
                    self.check_fsck(name, part)
                if "mtools" in self.readers:
                    self.check_mtools(name, img, want)
                if "pyfatfs" in self.readers:
                    self.check_pyfatfs(name, part, want)


# ------------------------------------------------------------ qemu + newlib
STARTUP = r"""
#include <stdint.h>
#include <sys/types.h>
extern int main(void);
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
void Reset_Handler(void){
  uint32_t *s=&_sidata,*d=&_sdata; while(d<&_edata)*d++=*s++;
  for(d=&_sbss;d<&_ebss;)*d++=0;
  *(volatile uint32_t*)0xE000ED88 |= (0xFU<<20);
  __asm volatile("dsb; isb");
  main(); for(;;);
}
void Def(void){for(;;);}
__attribute__((section(".isr_vector"))) const void* vt[16]={&_estack,Reset_Handler,Def,Def,Def,Def,Def,0,0,0,0,Def,Def,0,Def,Def};
static int semi(int op, void *arg){ register int r0 __asm("r0")=op; register void* r1 __asm("r1")=arg;
  __asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory"); return r0;}
extern char end; static char *brk=&end;
void *_sbrk(ptrdiff_t n){ char*p=brk; brk+=n; return p;}
int _write(int fd, const char*b, int n){ (void)fd; for(int i=0;i<n;i++){ char c=b[i]; semi(3,&c);} return n;}
void semi_exit(void){ semi(0x18,(void*)0x20026);}
"""

LINKER = """
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K }
_estack = ORIGIN(RAM)+LENGTH(RAM);
SECTIONS {
 .isr_vector : { KEEP(*(.isr_vector)) } >FLASH
 .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4);} >FLASH
 .ARM.exidx : { *(.ARM.exidx*) } >FLASH
 _sidata = LOADADDR(.data);
 .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >RAM AT>FLASH
 .bss : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; end = .; } >RAM
}
"""

BENCH = r"""
#include <stdio.h>
#define LOG_RING_IMPLEMENTATION
#include "log_ring.h"
#define VFAT_DEVICE_UID(w) ((w)[0] = 0x00280041U, (w)[1] = 0x30335111U, (w)[2] = 0x37383233U)
#define VFAT_IMPLEMENTATION
#include "vfat.h"
void semi_exit(void);
#define SYST ((volatile uint32_t*)0xE000E010)
static uint32_t now(void){ return SYST[2]; }
static uint32_t ticks(uint32_t a, uint32_t b){ return (a - b) & 0xFFFFFFU; }

#define NS 6U
static uint8_t flash[NS * 4096U];
static uint8_t f_read(uint32_t a, uint8_t *b, uint32_t n){ if (a + n > sizeof flash) return 0; memcpy(b, flash + a, n); return 1; }
static uint8_t f_prog(uint32_t a, const uint8_t *b, uint32_t n){ for (uint32_t i=0;i<n;i++) flash[a+i] &= b[i]; return 1; }
static uint8_t f_erase(uint32_t a){ memset(flash + a, 0xFF, 4096U); return 1; }
static const LogFlashOps OPS = { f_read, f_prog, f_erase, NULL, NULL };

static uint32_t rng = 99;
static uint32_t rnd(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float fb(uint32_t b){ float f; memcpy(&f, &b, 4); return f; }
static float rf(void){
  uint32_t h = rnd();
  switch (h % 8U) {
  case 0: return fb(rnd());
  case 1: return NAN;
  case 2: return fb(0xFFC00000U);
  case 3: return (h & 8U) ? -0.0f : INFINITY;
  case 4: return (float)((int32_t)(rnd() % 2000001U) - 1000000) / 8.0f;
  case 5: return (float)((int32_t)(rnd() % 200001U) - 100000) / 1000.0f + 0.0005f;
  default: return (float)((int32_t)(rnd() % 20001U) - 10000) / 100.0f;
  }
}
/* Строка с правдоподобными, но «длинными» значениями — для замера. */
static void real_meas(LogMeas *m, uint32_t id){
  memset(m, 0, sizeof *m);
  snprintf(m->ts, sizeof m->ts, "2026-10-%02u %02u:00:00", (unsigned)(id % 28U + 1U), (unsigned)(id % 24U));
  m->air_temp = -12.34f + (float)(id % 50U); m->air_hum = 55.55f; m->soil_temp = 18.75f; m->soil_hum = 41.2f;
  m->bat_valid = 1; m->bat_percent = 87; m->bat_volts = 3.912f; m->vdd = 3.297f; m->lse_ok = 1; m->time_ok = 1;
  m->sens_off = 360; m->write_fails = 123456; m->lowpower_skips = 3333; m->reset_cause = 0x0104;
  m->boot_count = 99999; m->soil_raw = 2345; m->bat_raw = 3011;
}
static LogRing ring;
static VFat vol;
static uint8_t sec[512];

int main(void){
  SYST[1] = 0xFFFFFF; SYST[2] = 0; SYST[0] = 5;
  uint32_t a = now(); __asm volatile("movw r0, #50000\n1: subs r0, r0, #1\n bne 1b":::"r0","cc");
  uint32_t calib = ticks(a, now());              /* 100 000 инструкций */
  char x[VFAT_LINE_BUF], y[VFAT_LINE_BUF];
  LogRecord r;
  unsigned mism = 0, total = 0;
  for (uint32_t i = 0; i < 4000U; i++) {
    LogMeas m; memset(&m, 0, sizeof m);
    snprintf(m.ts, sizeof m.ts, "2026-%02u-%02u 12:%02u:00", (unsigned)(rnd()%12U+1U), (unsigned)(rnd()%28U+1U), (unsigned)(rnd()%60U));
    m.air_temp = rf(); m.air_hum = rf(); m.soil_temp = rf(); m.soil_hum = rf();
    m.bat_valid = (uint8_t)(rnd() & 1U); m.bat_low = (uint8_t)(rnd() & 1U); m.bat_percent = (uint8_t)rnd();
    m.bat_volts = rf(); m.vdd = rf(); m.lse_ok = (uint8_t)(rnd()&1U); m.time_ok = (uint8_t)(rnd()&1U);
    m.aht_init_st = (int)(rnd()%4U); m.aht_read_st = (int)(rnd()%4U); m.ds_err = (uint8_t)(rnd()%16U);
    m.air_xchk = (uint8_t)(rnd()%4U); m.i2c_rec = (uint8_t)(rnd()%4U); m.sens_off = rnd()%70000U;
    m.write_fails = rnd(); m.lowpower_skips = rnd(); m.reset_cause = (uint16_t)rnd(); m.boot_count = rnd();
    m.soil_raw = rf(); m.bat_raw = rf();
    LogRecord_PackMeas(&r, &m);
    int n = VFat_FormatMeas(&r, x), k = VFat_FormatMeasSlow(&r, y);
    total++;
    if (n != k || (n > 0 && memcmp(x, y, (size_t)n) != 0)) mism++;
  }
  printf("MISMATCH %u OF %u\n", mism, total);

  /* Инструкции на строку: быстрый форматтер и snprintf. */
  LogMeas m; real_meas(&m, 7); LogRecord_PackMeas(&r, &m);
  a = now(); for (int i = 0; i < 20; i++) (void)VFat_FormatMeas(&r, x); uint32_t tf = ticks(a, now());
  a = now(); for (int i = 0; i < 20; i++) (void)VFat_FormatMeasSlow(&r, y); uint32_t ts = ticks(a, now());
  printf("LINE %s", x);
  /* CRC одной записи — его считает каждое чтение журнала при сборке. */
  volatile uint32_t sink = 0;
  a = now(); for (int i = 0; i < 20; i++) sink += LogRecord_Crc32(r.b, LOG_CRC_OFS); uint32_t tcrc = ticks(a, now());
  (void)sink;

  /* Журнал на NS секторов, том, худший сектор (поиск без курсора) и
   * последовательное чтение. */
  LogRing_Start(&ring, &OPS, NS, 0, 0);
  for (uint32_t i = 0; i < (NS - 2U) * 64U; i++) { real_meas(&m, i); LogRecord_PackMeas(&r, &m); LogRing_Append(&ring, &r); }
  a = now(); (void)VFat_Build(&vol, &ring, "bench"); uint32_t tb = ticks(a, now());
  uint32_t first = VFAT_PART_LBA + VFAT_DATA, nsec = (vol.size[0] + 511U) / 512U, worst = 0, seq = 0;
  for (uint32_t s = 0; s < nsec; s++) {
    vol.cur[0].valid = 0;
    a = now(); (void)VFat_Read(&vol, first + s, sec); uint32_t t = ticks(a, now());
    if (t > worst) worst = t;
  }
  vol.cur[0].valid = 0;
  a = now(); for (uint32_t s = 0; s < nsec; s++) (void)VFat_Read(&vol, first + s, sec); seq = ticks(a, now());
  printf("CALIB %lu FAST %lu SLOW %lu CRC %lu BUILD %lu RECS %lu WORST %lu SEQ %lu NSEC %lu\n",
         (unsigned long)calib, (unsigned long)(tf / 20U), (unsigned long)(ts / 20U),
         (unsigned long)(tcrc / 20U), (unsigned long)tb,
         (unsigned long)LogRing_Count(&ring), (unsigned long)worst, (unsigned long)seq, (unsigned long)nsec);
  semi_exit();
  return 0;
}
"""


class Newlib(unittest.TestCase):
    def test_newlib_nano(self):
        if not (have("arm-none-eabi-gcc") and have("qemu-system-arm")):
            self.skipTest("нет arm-none-eabi-gcc или qemu-system-arm: "
                          "apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi qemu-system-arm")
        tmp = tempfile.mkdtemp(prefix="vfat_arm_")
        try:
            for fn, text in (("start.c", STARTUP), ("l.ld", LINKER), ("bench.c", BENCH)):
                with open(os.path.join(tmp, fn), "w") as f:
                    f.write(text)
            elf = os.path.join(tmp, "b.elf")
            # Как в прошивке (.cproject): newlib-nano, -u _printf_float, -Os.
            subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16",
                            "-mfloat-abi=hard", "-Os", "-I" + os.path.join(ROOT, "Core", "Inc"),
                            "--specs=nano.specs", "--specs=nosys.specs", "-u", "_printf_float",
                            "-nostartfiles", "-T", os.path.join(tmp, "l.ld"),
                            os.path.join(tmp, "start.c"), os.path.join(tmp, "bench.c"), "-lm", "-o", elf],
                           check=True, capture_output=True)
            r = subprocess.run(["qemu-system-arm", "-M", "netduinoplus2", "-nographic",
                                "-semihosting-config", "enable=on,target=native", "-icount", "shift=0",
                                "-kernel", elf, "-monitor", "none", "-serial", "none"],
                               capture_output=True, text=True, timeout=300)
            out = r.stdout + r.stderr
            mism = [l for l in out.splitlines() if l.startswith("MISMATCH")]
            self.assertTrue(mism, "нет вывода из qemu:\n" + out[-2000:])
            self.assertEqual(mism[0].split()[1], "0", "newlib-nano печатает иначе: " + mism[0])
            st = dict(zip(*[iter(next(l for l in out.splitlines() if l.startswith("CALIB")).split())] * 2))
            k = 100000.0 / int(st["CALIB"])                 # инструкций на тик SysTick
            ins = {n: int(st[n]) * k for n in ("FAST", "SLOW", "CRC", "BUILD", "WORST", "SEQ")}
            nsec, recs = int(st["NSEC"]), int(st["RECS"])
            print("\n  newlib-nano, Cortex-M4 (инструкций): строка — своя %.0f, snprintf %.0f;"
                  " CRC записи %.0f; сборка — %.0f на запись; сектор — худший %.0f, подряд %.0f"
                  % (ins["FAST"], ins["SLOW"], ins["CRC"], ins["BUILD"] / recs, ins["WORST"],
                     ins["SEQ"] / nsec),
                  file=sys.stderr)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


# Класс MSC ST собирается на хосте с заглушками LL: READ10 на 4 сектора, отказ
# Read на k-м секторе (V1, П5б+). По BOT ошибка в фазе данных — STALL IN, а
# CSW FAILED (с остатком) — только после CLEAR_FEATURE.
BOT_CONF = r"""/* Заглушка usbd_conf.h для хостовой сборки класса MSC. */
#ifndef __USBD_CONF__H__
#define __USBD_CONF__H__
#include <stdint.h>
#include <string.h>
#define USBD_MAX_NUM_INTERFACES 1U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 512U
#define USBD_DEBUG_LEVEL 0U
#define USBD_LPM_ENABLED 0U
#define USBD_SELF_POWERED 1U
#define MSC_MEDIA_PACKET 512U
#define DEVICE_FS 0
#define USBD_malloc(x) malloc(x)
#define USBD_free(x) free(x)
#define USBD_memset memset
#define USBD_memcpy memcpy
#define USBD_Delay(x)
#define USBD_UsrLog(...)
#define USBD_ErrLog(...)
#define USBD_DbgLog(...)
#include <stdlib.h>
#define __IO volatile
#define UNUSED(x) ((void)(x))
#define __STATIC_INLINE static inline
#define __PACKED __attribute__((packed))
#define __ALIGN_BEGIN
#define __ALIGN_END
#endif
"""

BOT_BENCH = r"""#include <stdio.h>
#include "usbd_msc.h"
uint8_t MSCInEpAdd = 0x81U, MSCOutEpAdd = 0x01U;
static int stall_in, tx_n, tx_len[16];
static uint8_t tx_buf[16][16];
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *p, uint8_t ep){ (void)p; if (ep & 0x80U) stall_in++; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *p, uint8_t ep){ (void)p; (void)ep; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *p, uint8_t ep){ (void)p; (void)ep; return USBD_OK; }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *p, uint8_t ep){ (void)p; (void)ep; return 31U; }
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *p, uint8_t ep, uint8_t *b, uint32_t n){ (void)p; (void)ep; (void)b; (void)n; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *p, uint8_t ep, uint8_t *b, uint32_t n){
  (void)p; (void)ep;
  if (tx_n < 16) { tx_len[tx_n] = (int)n; memcpy(tx_buf[tx_n], b, n < 16U ? n : 16U); tx_n++; }
  return USBD_OK;
}
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *p, uint8_t ep){ (void)p; (void)ep; return 0; }
USBD_StatusTypeDef USBD_CtlSendData(USBD_HandleTypeDef *p, uint8_t *b, uint32_t n){ (void)p; (void)b; (void)n; return USBD_OK; }

static int fail_at = -1;
static int8_t s_init(uint8_t l){ (void)l; return 0; }
static int8_t s_cap(uint8_t l, uint32_t *n, uint16_t *s){ (void)l; *n = 32768U; *s = 512U; return 0; }
static int8_t s_ready(uint8_t l){ (void)l; return 0; }
static int8_t s_wp(uint8_t l){ (void)l; return 1; }
static int8_t s_read(uint8_t l, uint8_t *b, uint32_t a, uint16_t n){ (void)l; (void)n; memset(b, (int)a, 512); return ((int)a == fail_at) ? -1 : 0; }
static int8_t s_write(uint8_t l, uint8_t *b, uint32_t a, uint16_t n){ (void)l; (void)b; (void)a; (void)n; return -1; }
static int8_t s_maxlun(void){ return 0; }
static int8_t inq[36];
static USBD_StorageTypeDef st = { s_init, s_cap, s_ready, s_wp, s_read, s_write, s_maxlun, inq };

static USBD_HandleTypeDef dev;
static USBD_MSC_BOT_HandleTypeDef h;

/* READ10 на 4 сектора с LBA 100; отказ чтения на fail. Печатает, что увидел хост. */
static void run(int fail){
  memset(&dev, 0, sizeof dev); memset(&h, 0, sizeof h);
  dev.pClassDataCmsit[0] = &h; dev.pUserData[0] = &st; dev.dev_speed = USBD_SPEED_FULL;
  stall_in = 0; tx_n = 0; fail_at = fail;
  MSC_BOT_Init(&dev);
  h.scsi_medium_state = SCSI_MEDIUM_UNLOCKED;
  h.scsi_blk[0].nbr = 32768U; h.scsi_blk[0].size = 512U;      /* как после READ CAPACITY */
  h.cbw.dSignature = USBD_BOT_CBW_SIGNATURE; h.cbw.dTag = 0x1234; h.cbw.dDataLength = 4U * 512U;
  h.cbw.bmFlags = 0x80U; h.cbw.bLUN = 0; h.cbw.bCBLength = 10;
  memset(h.cbw.CB, 0, 16); h.cbw.CB[0] = 0x28; h.cbw.CB[5] = 100; h.cbw.CB[8] = 4;
  MSC_BOT_DataOut(&dev, 1);                         /* CBW принят, первый сектор ушёл */
  for (int i = 0; i < 6 && !stall_in; i++)
  {
    int before = tx_n;
    MSC_BOT_DataIn(&dev, 0x81);                     /* пакет ушёл хосту */
    if (tx_n == before) break;
    if (tx_len[tx_n - 1] == 13) break;              /* CSW */
  }
  int stalled = stall_in;
  int early = 0;                                    /* CSW ушёл в фазе данных, без STALL */
  for (int i = 0; i < tx_n; i++) if (tx_len[i] == 13) early = 1;
  if (stalled) MSC_BOT_CplClrFeature(&dev, 0x81);   /* хост: CLEAR_FEATURE(HALT) */
  int data = 0;
  for (int i = 0; i < tx_n; i++) if (tx_len[i] == 512) data++;
  int last = tx_n - 1;
  uint32_t residue = 0; uint8_t status = 0xFF;
  if (last >= 0 && tx_len[last] == 13) { memcpy(&residue, tx_buf[last] + 8, 4); status = tx_buf[last][12]; }
  printf("FAIL %d DATA %d STALL %d EARLY %d STATUS %u RESIDUE %lu\n", fail, data, stalled, early,
         (unsigned)status, (unsigned long)residue);
}
int main(void){ run(-1); run(100); run(102); return 0; }
"""


class MscBot(unittest.TestCase):
    def test_read_fail_stalls_in(self):
        if not have("gcc"):
            self.skipTest("нет gcc")
        lib = os.path.join(ROOT, "Middlewares", "ST", "STM32_USB_Device_Library")
        tmp = tempfile.mkdtemp(prefix="msc_bot_")
        try:
            for fn, text in (("usbd_conf.h", BOT_CONF), ("bench.c", BOT_BENCH)):
                with open(os.path.join(tmp, fn), "w") as f:
                    f.write(text)
            exe = os.path.join(tmp, "b")
            src = [os.path.join(lib, "Class", "MSC", "Src", n)
                   for n in ("usbd_msc_bot.c", "usbd_msc_scsi.c", "usbd_msc_data.c")]
            subprocess.run(["gcc", "-std=c11", "-I" + tmp, "-I" + os.path.join(lib, "Class", "MSC", "Inc"),
                            "-I" + os.path.join(lib, "Core", "Inc"), "-o", exe,
                            os.path.join(tmp, "bench.c")] + src, check=True, capture_output=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
            res = {}
            for line in out.splitlines():
                kv = dict(zip(*[iter(line.split())] * 2))
                res[int(kv["FAIL"])] = {k: int(v) for k, v in kv.items()}
            self.assertEqual((res[-1]["DATA"], res[-1]["STALL"], res[-1]["STATUS"]), (4, 0, 0), out)
            # Отказ первого сектора и посреди команды: STALL, CSW не раньше
            # CLEAR_FEATURE, статус FAILED, остаток — непереданные секторы.
            for fail, sent in ((100, 0), (102, 2)):
                r = res[fail]
                self.assertEqual((r["DATA"], r["STALL"], r["EARLY"], r["STATUS"], r["RESIDUE"]),
                                 (sent, 1, 0, 1, (4 - sent) * 512), "отказ на LBA %d: %s" % (fail, out))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
