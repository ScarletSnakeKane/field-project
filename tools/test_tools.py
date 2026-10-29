# -*- coding: utf-8 -*-
"""Самопроверка инструментов: плата не нужна.

    python tools/test_tools.py

Проверяется то, на чём инструменты уже ошибались (находки D1, D2, D3, D5, D8
аудита и R1, R4–R6, R9 приёмки), и устройство архива П5: вставка дампера не
меняет окончания строк и снимается без следа; дампер стирает флеш, только
если она совпала с образом, который хост сохранил целиком; разбор образа
журнала даёт те же data.txt и data_old.txt, что том USB, а его таблица полей
— та же, что в Core/Inc/log_record.h; копия с тома сверяется с info.txt;
срок батареи считается по кривой прошивки от текущего заряда; nan не
портит диапазоны; испорченное чтение ОЗУ не попадает на диск.

C-части (вставка дампера с заглушками SPI, эталон журнала tools/log_oracle.c
на настоящих log_ring.h и vfat.h) собираются хостовым gcc. Где gcc нет (у
владельца тесты идут под MSVC), эти проверки пропускаются; таблица полей
сверяется с log_record.h и без gcc — по тексту заголовка.
"""
import ctypes
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import analyze_run  # noqa: E402
import dump_patch   # noqa: E402
import log_image    # noqa: E402

DATA = os.path.join(ROOT, "audit", "data", "nan_first.txt")
GCC = shutil.which("gcc")


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def spit(path, data, mode="wb"):
    with open(path, mode) as f:
        f.write(data)


EXE = ".exe" if os.name == "nt" else ""


def fnv1a(data):
    h = 2166136261
    for b in bytearray(data):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def run_tool(name, *args, cwd=None):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, name)] + list(args),
                       cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode("utf-8"), p.stderr.decode("utf-8")


class SrcTree:
    """Временная копия main.c: dump_patch.py работает по относительному пути."""

    def __init__(self, eol):
        self.dir = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.dir, "Core", "Src"))
        self.path = os.path.join(self.dir, "Core", "Src", "main.c")
        with io.open(os.path.join(ROOT, "Core", "Src", "main.c"), encoding="utf-8", newline="") as f:
            s = f.read().replace("\r\n", "\n")
        self.orig = s.replace("\n", eol).encode("utf-8")
        spit(self.path, self.orig)

    def read(self):
        return slurp(self.path)

    def patch(self, *args):
        return run_tool("dump_patch.py", *args, cwd=self.dir)

    def close(self):
        shutil.rmtree(self.dir)


class PatchEol(unittest.TestCase):
    """D5: патч не переписывает окончания строк и откатывается байт-в-байт."""

    def check(self, eol):
        t = SrcTree(eol)
        try:
            self.assertEqual(t.patch("--wipe", "123", "0x1")[0], 0)
            d = t.read()
            if eol == "\r\n":
                self.assertEqual(d.count(b"\n"), d.count(b"\r\n"), "голые LF в CRLF-файле")
            else:
                self.assertNotIn(b"\r", d, "CR в LF-файле")
            self.assertIn("ВРЕМЕННЫЙ ДАМПЕР".encode("utf-8"), d)
            self.assertEqual(t.patch("--revert")[0], 0)
            self.assertEqual(t.read(), t.orig)
        finally:
            t.close()

    def test_lf(self):
        self.check("\n")

    def test_crlf(self):
        self.check("\r\n")

    def test_revert_keeps_foreign_edits(self):
        t = SrcTree("\n")
        try:
            t.patch()
            spit(t.path, b"/* edit */\n", "ab")
            t.patch("--revert")
            self.assertEqual(t.read(), t.orig + b"/* edit */\n")
        finally:
            t.close()


@unittest.skipUnless(GCC and os.path.isdir(os.path.join(ROOT, "Drivers")),
                     "нет gcc или исходников HAL — компиляция вставки в main.c пропущена")
class PatchCompiles(unittest.TestCase):
    """Вставка дампера встаёт в текущий main.c и компилируется с настоящими
    заголовками HAL — теми же флагами, что tools/syncheck.sh (П4: якоря
    и вызовы вроде dump_serve_raw уходили бы иначе незамеченными до сборки
    у владельца)."""

    def compile(self, *patch_args):
        with open(os.path.join(TOOLS, "syncheck.sh"), encoding="utf-8") as f:
            sync = f.read()
        inc = re.search(r'^INC="([^"]*)"', sync, re.M).group(1).split()
        t = SrcTree("\n")
        try:
            self.assertEqual(t.patch(*patch_args)[0], 0)
            # -S, а не -fsyntax-only: без кодогенерации gcc не говорит о
            # неиспользуемых static-функциях, а сборка в CubeIDE скажет.
            # Ассемблер хоста инструкции Cortex-M не знает — до него не доходим.
            p = subprocess.run([GCC, "-S", "-o", os.devnull, "-std=gnu11", "-DSTM32F401xE", "-DUSE_HAL_DRIVER",
                                "-Wall", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
                                "-Wno-overflow"] + inc + [t.path],
                               cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            err = p.stderr.decode("utf-8", "replace")
            self.assertEqual(p.returncode, 0, err)
            self.assertNotIn("warning", err)
            self.assertEqual(t.patch("--revert")[0], 0)
            self.assertEqual(t.read(), t.orig)
        finally:
            t.close()

    def test_read_only(self):
        self.compile()

    def test_wipe(self):
        self.compile("--wipe", "1000", "0x1234ABCD")


# Заглушки ровно под то, что использует вставка дампера. Флеш нарочно не
# кратна куску 32 КБ (12 секторов = 48 КБ), чтобы проверить хвост.
STUBS = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define FLASH_SIZE_BYTES (12 * 4096)
static uint8_t g_flash[FLASH_SIZE_BYTES];
static int g_reads = 0, g_read_fail = -1, g_erase_fail = -1, g_unlock_fail = 0, g_locked = 0;
static int g_erases = 0, g_unlocks = 0;
static uint8_t SPI_Flash_Read(uint32_t a, uint8_t *b, uint32_t n)
{
    if (g_reads++ == g_read_fail || a + n > FLASH_SIZE_BYTES || n > 4096) return 0;   /* за край — как у драйвера, отказ */
    memcpy(b, g_flash + a, n);
    return 1;
}
static uint8_t SPI_Flash_EraseSector(uint32_t a)
{
    g_erases++;
    if ((int)(a / 4096) == g_erase_fail) return 0;
    /* Запертая SST26 отвечает «успехом», ничего не стерев (T2 отчёта П5а). */
    if (!g_locked) memset(g_flash + a, 0xFF, 4096);
    return 1;
}
static uint8_t SPI_Flash_Unlock(void) { g_unlocks++; return (uint8_t)!g_unlock_fail; }
static struct { volatile uint32_t KR; } g_iwdg;
#define IWDG (&g_iwdg)
#define HAL_GPIO_WritePin(a, b, c) ((void)0)
#define HAL_Delay(x) ((void)0)
#define SENSOR_POWERUP_DELAY_MS 0
typedef struct { float volts, vdd; uint8_t percent, low, valid; } Battery_Data;
static int g_bat_valid = 1;
static float Soil_ReadVoltage(void) { return 1.0f; }
static void Battery_Read(Battery_Data *b)
{
    /* Как battery.c после П2: при отказе NAN и 255 %. */
    b->valid = (uint8_t)g_bat_valid; b->low = 0;
    b->volts = g_bat_valid ? 3.8f : NAN; b->vdd = g_bat_valid ? 3.3f : NAN;
    b->percent = g_bat_valid ? 65 : 255;
}
static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261U;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619U; }
    return h;
}
"""

MAIN = r"""
#define W(f) (unsigned)(offsetof(dump_info_t, f) / 4)
int main(int argc, char **argv)
{
    if (strcmp(argv[1], "layout") == 0)
    {
        /* Номера слов dump_info_t: read_data.sh читает их по номерам. */
        printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
               W(magic), W(raw_total), W(soil_mv), W(bat_mv), W(vdd_mv), W(bat_pct),
               W(flash_hash), W(hash_ok), W(wipe), W(raw_req), W(raw_done), W(raw_off),
               W(raw_len), W(raw_hash), (unsigned)sizeof(dump_info_t));
        return 0;
    }
    /* argv[1]: флаги "bat=0", "rfail=N", "efail=S", "ufail", "locked" через запятую или "-". */
    for (char *t = strtok(argv[1], ","); t; t = strtok(NULL, ","))
    {
        if (strcmp(t, "bat=0") == 0) g_bat_valid = 0;
        else if (strncmp(t, "rfail=", 6) == 0) g_read_fail = atoi(t + 6);
        else if (strncmp(t, "efail=", 6) == 0) g_erase_fail = atoi(t + 6);
        else if (strcmp(t, "ufail") == 0) g_unlock_fail = 1;
        else if (strcmp(t, "locked") == 0) g_locked = 1;
    }
    for (uint32_t i = 0; i < FLASH_SIZE_BYTES; i++) g_flash[i] = (uint8_t)(i * 7 + 3);
    uint32_t before = fnv(g_flash, FLASH_SIZE_BYTES);
    run_dump();
    printf("D %u %u %u %u %u %u %u %u %u %u %u %u\n", (unsigned)dump_info.magic, (unsigned)dump_info.raw_total,
           (unsigned)dump_info.flash_hash, (unsigned)dump_info.hash_ok, (unsigned)dump_info.wipe,
           (unsigned)g_erases, (unsigned)g_unlocks, (unsigned)before, (unsigned)fnv(g_flash, FLASH_SIZE_BYTES),
           (unsigned)dump_info.bat_mv, (unsigned)dump_info.vdd_mv, (unsigned)dump_info.bat_pct);
    /* Дальше — запросы сырых кусков, как их шлёт хост: "N", "!N" (чтение
     * флеш откажет) или "=N" (хост сначала обнулил raw_done — повтор).
     * Шаг без нового запроса не должен ничего менять. */
    for (int i = 2; i < argc; i++)
    {
        const char *a = argv[i];
        g_read_fail = -1;
        if (a[0] == '!') { g_read_fail = g_reads; a++; }
        if (a[0] == '=') { dump_info.raw_done = 0; a++; }
        dump_info.raw_req = (uint32_t)atoi(a);
        dump_raw_step();
        uint32_t len = dump_info.raw_len;
        dump_info.raw_len = 0xDEADU;
        dump_raw_step();   /* повтор того же запроса — ничего не трогает */
        int untouched = (dump_info.raw_len == 0xDEADU);
        printf("R %u %u %u %u %d\n", (unsigned)dump_info.raw_off, (unsigned)len,
               (unsigned)dump_info.raw_hash, (unsigned)dump_info.raw_done, untouched);
        if (len && len != 0xDEADU && memcmp(dump_buf, g_flash + dump_info.raw_off, len) != 0)
            printf("BUF MISMATCH\n");
    }
    return 0;
}
"""

STUB_FLASH = bytes((i * 7 + 3) & 0xFF for i in range(12 * 4096))


@unittest.skipUnless(GCC, "нет gcc — проверка C-вставки дампера пропущена")
class Dumper(unittest.TestCase):
    """Дампер под журнал: сырой образ кусками, хеш всей флеш, стирание
    только той флеш, образ которой хост сохранил (D1 в новом виде), и
    живые напряжения. Собирается ровно тот текст, что вставляет патч."""

    def build(self, *patch_args):
        t = SrcTree("\n")
        try:
            rc, out, err = t.patch(*patch_args)
            self.assertEqual(rc, 0, err)
            s = t.read().decode("utf-8")
        finally:
            t.close()
        blocks = re.findall(re.escape(dump_patch.BEGIN) + r".*?" + re.escape(dump_patch.END), s, re.S)
        self.assertEqual(len(blocks), 2)
        decl, dump = blocks
        self.assertIn("dump_serve_raw();", dump)
        # Вечный цикл раздачи сырых кусков проверяется по шагам (dump_raw_step).
        dump = dump.replace("dump_serve_raw();", "if (0) { dump_serve_raw(); }\n      return;")
        src = STUBS + decl + "\nstatic void run_dump(void)\n{\n  " + dump + "\n}\n" + MAIN
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t" + EXE)
        spit(c, src.encode("utf-8"))
        subprocess.check_call([GCC, "-std=c11", "-Wall", "-Werror", "-o", exe, c])
        return exe

    def run_exe(self, exe, flags="-", *reqs):
        out = subprocess.check_output([exe, flags] + list(reqs)).decode()
        d = [l for l in out.splitlines() if l.startswith("D ")][0].split()[1:]
        magic, total, h, hok, wipe, erases, unlocks, before, after, bmv, vmv, pct = map(int, d)
        r = dict(magic=magic, total=total, hash=h, hash_ok=hok, wipe=wipe, erases=erases,
                 unlocks=unlocks, before=before, after=after, bat=(bmv, vmv, pct))
        r["raw"] = [tuple(map(int, l.split()[1:])) for l in out.splitlines() if l.startswith("R ")]
        self.assertNotIn("BUF MISMATCH", out)
        return r

    def wipe_exe(self, size=len(STUB_FLASH), h=None):
        return self.build("--wipe", str(size), hex(fnv1a(STUB_FLASH) if h is None else h))

    def test_read_only_never_erases(self):
        r = self.run_exe(self.build())
        self.assertEqual(r["magic"], 0xD00DFEED)
        self.assertEqual((r["total"], r["hash"], r["hash_ok"]), (len(STUB_FLASH), fnv1a(STUB_FLASH), 1))
        self.assertEqual((r["wipe"], r["erases"], r["unlocks"]), (0, 0, 0))
        self.assertEqual(r["after"], r["before"])

    def test_wipe_when_matches(self):
        r = self.run_exe(self.wipe_exe())
        self.assertEqual(r["wipe"], 1)
        self.assertEqual(r["erases"], 12)
        self.assertEqual(r["after"], fnv1a(b"\xff" * len(STUB_FLASH)), "флеш стёрта целиком")
        self.assertEqual(r["hash"], fnv1a(STUB_FLASH), "хеш — флеш до стирания")

    def test_wipe_refused_on_hash_mismatch(self):
        r = self.run_exe(self.wipe_exe(h=fnv1a(STUB_FLASH) ^ 1))
        self.assertEqual((r["wipe"], r["erases"], r["unlocks"]), (2, 0, 0))
        self.assertEqual(r["after"], r["before"])

    def test_wipe_refused_on_size_mismatch(self):
        # Хеш совпадает, размер нет: отказ только по размеру.
        r = self.run_exe(self.wipe_exe(size=len(STUB_FLASH) - 4096))
        self.assertEqual((r["wipe"], r["erases"]), (2, 0))

    def test_wipe_refused_on_read_error(self):
        # Флеш не прочиталась при подсчёте хеша — сверить нечем, не трогаем.
        r = self.run_exe(self.wipe_exe(), "rfail=5")
        self.assertEqual((r["hash_ok"], r["wipe"], r["erases"]), (0, 4, 0))
        self.assertEqual(r["after"], r["before"])

    def test_wipe_unlock_failed(self):
        r = self.run_exe(self.wipe_exe(), "ufail")
        self.assertEqual((r["wipe"], r["erases"]), (3, 0))
        self.assertEqual(r["after"], r["before"])

    def test_wipe_erase_failed_counted(self):
        r = self.run_exe(self.wipe_exe(), "efail=4")
        self.assertEqual(r["wipe"], 3 | (1 << 8))
        self.assertEqual(r["erases"], 12, "остальные секторы всё равно стираются")

    def test_wipe_locked_chip_caught(self):
        # Стирание «успешно», но флеш заперта: ловит только проверка чтением.
        r = self.run_exe(self.wipe_exe(), "locked")
        self.assertEqual(r["wipe"], 3 | (12 << 8))

    def test_battery_fail_marked(self):
        # R6: отказ замера батареи — 0xFFFFFFFF, а не 0 мВ и 255 %.
        exe = self.build()
        self.assertEqual(self.run_exe(exe)["bat"], (3800, 3300, 65))
        self.assertEqual(self.run_exe(exe, "bat=0")["bat"], (0xFFFFFFFF,) * 3)

    def test_raw_image(self):
        # Флеш отдаётся кусками, и из них unpack_dump.py --raw собирает образ байт в байт.
        exe = self.build()
        r = self.run_exe(exe, "-", "1", "2", "3")["raw"]
        self.assertEqual(r[0], (0, 32768, fnv1a(STUB_FLASH[:32768]), 1, 1))
        self.assertEqual(r[1], (32768, len(STUB_FLASH) - 32768, fnv1a(STUB_FLASH[32768:]), 2, 1), "хвост флеш")
        self.assertEqual(r[2][:2], (65536, 0), "кусок за краем — 0 байт")
        self.assertEqual(r[2][3], 3)

        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        img = os.path.join(d, "flash.img")
        for (off, ln, h, _, _), part in ((r[1], STUB_FLASH[32768:]), (r[0], STUB_FLASH[:32768])):
            src = os.path.join(d, "part.bin")
            spit(src, part)
            # Как печатает -r32: hex без 0x, с ведущими нулями.
            self.assertEqual(run_tool("unpack_dump.py", "--raw", src, img, "%08X" % off, "%08X" % h)[0], 0)
        self.assertEqual(slurp(img), STUB_FLASH)

        # Испорченный кусок в образ не попадает.
        spit(os.path.join(d, "bad.bin"), b"x" * 10)
        rc = run_tool("unpack_dump.py", "--raw", os.path.join(d, "bad.bin"), img, "0", "%08X" % r[0][2])[0]
        self.assertNotEqual(rc, 0)
        self.assertEqual(slurp(img), STUB_FLASH)

    def test_raw_read_failure_and_repeat(self):
        exe = self.build()
        r = self.run_exe(exe, "-", "!1", "1", "=1", "2")["raw"]
        # Отказ чтения — 0 байт и хеш пустого куска, а не чужой буфер.
        self.assertEqual(r[0][:4], (0, 0, fnv1a(b""), 1))
        self.assertTrue(all(x[4] == 1 for x in r), "повтор того же запроса не перечитывает")
        # Тот же номер без сброса raw_done не выполняется...
        self.assertEqual(r[1][1], 0xDEAD)
        # ...а со сбросом (так повторяет read_data.sh) — выполняется честно.
        self.assertEqual(r[2][:2], (0, 32768))
        self.assertEqual(r[3][1], len(STUB_FLASH) - 32768)

    def test_layout_for_read_data(self):
        # read_data.sh читает 14 слов по номерам, raw_req — 0x24, raw_done — 0x28.
        out = subprocess.check_output([self.build(), "layout"]).decode().split()
        self.assertEqual(list(map(int, out)), list(range(14)) + [56])

    def test_feeds_watchdog(self):
        # Хеш всей флеш, стирание (до и после каждого сектора), кусок и
        # вечный цикл раздачи — везде пинок IWDG, иначе ~30 с и перезапуск.
        self.assertEqual(dump_patch.DECL.count("IWDG->KR = 0x0000AAAAU;"), 5)

    def test_placed_before_journal_open(self):
        # Дампер стоит после инициализации флеш и до открытия журнала: открытие
        # на первом старте переносит старый том (пишет), а первая запись
        # стирает запасной сектор — снятый образ был бы уже не тем.
        t = SrcTree("\n")
        try:
            self.assertEqual(t.patch()[0], 0)
            s = t.read().decode("utf-8")
        finally:
            t.close()
        body = s[s.index("int main(void)"):]
        dump = body.index(dump_patch.BEGIN)
        self.assertLess(body.rindex("SPI_Flash_Init();", 0, dump), dump)
        self.assertLess(dump, body.index("Archive_Open(boot_power_ok)"))
        self.assertNotIn("Archive_Open", body[:dump])
        self.assertNotIn("LogRing_", body[:dump])


# ------------------------------------------------- формат записи журнала

def src_text(*parts):
    with io.open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def c_define(src, name):
    m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)U?\b" % name, src)
    return int(m.group(1), 0) if m else None


class LogFormat(unittest.TestCase):
    """Таблица полей log_image.py — та же, что в Core/Inc/log_record.h.
    Текстом заголовка (работает и без gcc); байт в байт — в LogImage."""

    def setUp(self):
        self.h = src_text("Core", "Inc", "log_record.h")

    def test_constants(self):
        for c, py in (("LOG_RECORD_SIZE", log_image.RECORD_SIZE), ("LOG_PAYLOAD_OFS", log_image.PAYLOAD_OFS),
                      ("LOG_PAYLOAD_SIZE", log_image.PAYLOAD_SIZE), ("LOG_CRC_OFS", log_image.CRC_OFS),
                      ("LOG_VERSION", log_image.VERSION), ("LOG_TYPE_MEAS", log_image.TYPE_MEAS),
                      ("LOG_TYPE_TEXT", log_image.TYPE_TEXT), ("LOG_TYPE_IMPORT_FILE", log_image.TYPE_IMPORT_FILE),
                      ("LOG_TYPE_IMPORT_END", log_image.TYPE_IMPORT_END), ("LOG_TEXT_MAX", log_image.TEXT_MAX)):
            self.assertEqual(c_define(self.h, c), py, c)
        ring = src_text("Core", "Inc", "log_ring.h")
        self.assertEqual(c_define(ring, "LOG_SECTOR_SIZE"), log_image.SECTOR_SIZE)
        self.assertEqual(c_define(ring, "LOG_FLASH_SECTORS"), log_image.FLASH_SECTORS)
        self.assertEqual(c_define(src_text("Core", "Inc", "vfat.h"), "VFAT_LINE_BUF") - 1, log_image.LINE_LIMIT)
        self.assertIn('#define LOG_TS_UNKNOWN "%s"' % log_image.TS_UNKNOWN, self.h)

    def test_header(self):
        self.assertEqual(log_image.COLUMNS, firmware_header())

    def test_meas_layout(self):
        # Смещения и виды полей — из LogRecord_PackMeas, где запись собирается.
        body = self.h[self.h.index("LogRecord_PackMeas(LogRecord *r"):]
        body = body[:body.index("\n}\n")]
        got = {"ts": (0, "ts")}
        for fn, ofs, name in re.findall(r"LogRecord_(PutF|Put16|Put32)\(p \+ (\d+),\s*(.*?)\);", body):
            m = re.search(r"m->(\w+)", name)
            field = m.group(1) if m else name.strip()
            kind = {"PutF": "f32", "Put32": "u32", "Put16": "u16"}[fn]
            if "PackRaw" in name:
                kind = "raw16"
            if field == "bits":
                kind = "bits"
            got[field] = (int(ofs), kind)
        for ofs, field in re.findall(r"p\[(\d+)\] = m->(\w+);", body):
            got[field] = (int(ofs), "u8")
        want = {name: (ofs, kind) for name, ofs, kind in log_image.MEAS_FIELDS}
        self.assertEqual(got, want)
        # Ни одно поле не заходит на соседнее и за нагрузку.
        size = {"ts": 6, "f32": 4, "u32": 4, "u16": 2, "raw16": 2, "bits": 2, "u8": 1}
        spans = sorted((o, o + size[k]) for o, k in want.values())
        for (a0, a1), (b0, _) in zip(spans, spans[1:]):
            self.assertLessEqual(a1, b0)
        self.assertLessEqual(spans[-1][1], log_image.PAYLOAD_SIZE)

    def test_meas_bits(self):
        body = self.h[self.h.index("uint32_t bits = (m->bat_valid"):]
        body = body[:body.index(";")]
        got = {}
        for f, v in re.findall(r"\(m->(\w+)\s*\?\s*(\d+)U\s*:\s*0U\)", body):
            got[f] = (int(v).bit_length() - 1, 1)
        for f, mx, sh in re.findall(r"LogRecord_(?:Code|Sat)\(m->(\w+),\s*(\d+)U\)\s*<<\s*(\d+)", body):
            got[f] = (int(sh), int(mx).bit_length())
        self.assertEqual(got, {n: (lo, w) for n, lo, w in log_image.MEAS_BITS})

    def test_crc(self):
        # LogRecord_Crc32 — CRC32 IEEE с отражением (0xEDB88320), как у zip,
        # по полубайтам (П5б+). Без gcc его не вызвать, поэтому сверяем сам
        # текст функции: таблица — ровно полубайтовая таблица этого полинома,
        # начальное значение и финальная инверсия — как у zip. Разбор образа
        # считает CRC через zlib, и это сходится, только если всё три верно.
        m = re.search(r"uint32_t\s+LogRecord_Crc32\s*\([^)]*\)\s*\{(.*?)\n\}", self.h, re.S)
        self.assertIsNotNone(m, "не найден LogRecord_Crc32 в log_record.h")
        body = m.group(1)
        t = re.search(r"t\[16\]\s*=\s*\{([^}]*)\}", body)
        self.assertIsNotNone(t, "таблица t[16] в LogRecord_Crc32 не найдена")
        got = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})UL", t.group(1))]
        want = []
        for n in range(16):
            c = n
            for _ in range(4):
                c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
            want.append(c)
        self.assertEqual(got, want, "таблица CRC в прошивке не та, что у zip")
        self.assertRegex(body, r"c\s*=\s*0xFFFFFFFFUL", "начальное значение CRC не 0xFFFFFFFF")
        self.assertRegex(body, r"return\s+~c\s*;", "нет финальной инверсии CRC")
        self.assertEqual(len(re.findall(r"c\s*=\s*\(c\s*>>\s*4\)\s*\^\s*t\[c\s*&\s*15U?\]", body)), 2,
                         "на байт должно быть два шага по полубайту")
        self.assertEqual(log_image.crc32(b"123456789"), 0xCBF43926)


ORACLE_SCENARIOS = ("empty", "few", "rings", "seqwrap", "damaged", "restart", "torn_erase",
                    "import", "import_pending", "foreign")


@unittest.skipUnless(GCC, "нет gcc — сверка разбора образа с прошивочным журналом пропущена")
class LogImage(unittest.TestCase):
    """Сырой образ журнала, записанного настоящим log_ring.h, -> те же
    data.txt и data_old.txt, что отдаёт настоящий vfat.h (tools/log_oracle.c):
    круги кольца, переход номеров через 2^32, испорченные слоты и сектор,
    оборванное стирание, перезапуски, импорт старого архива, чужая флеш.
    Случайные замеры — все края форматтера (nan, -nan, inf, -0, огромные и
    субнормальные float, насыщение кодов, неразобранная метка)."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        exe = os.path.join(cls.dir, "oracle" + EXE)
        subprocess.check_call([GCC, "-std=c11", "-O1", "-Wall", "-I", os.path.join(ROOT, "Core", "Inc"),
                               "-o", exe, os.path.join(TOOLS, "log_oracle.c"), "-lm"])
        cls.info = {}
        for sc in ORACLE_SCENARIOS:
            out = subprocess.check_output([exe, sc, cls.dir]).decode().split()
            cls.info[sc] = dict(kv.split("=") for kv in out[1:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def path(self, sc, suf):
        return os.path.join(self.dir, sc + suf)

    def want(self, sc, suf):
        p = self.path(sc, suf)
        return slurp(p) if os.path.exists(p) else None

    def check(self, sc):
        j = log_image.Journal(slurp(self.path(sc, ".img")))
        vol = log_image.Volume(j)
        info = self.info[sc]
        if j.state == "ok":
            self.assertEqual((j.count, j.tail_seq), (int(info["count"]), int(info["first"])), "голова/хвост")
        self.assertEqual(self.want(sc, ".data.txt"), vol.data, "data.txt")
        self.assertEqual(self.want(sc, ".old.txt"), vol.old or None, "data_old.txt")
        self.assertEqual((vol.n_meas, vol.n_bad + vol.n_skip), (int(info["meas"]), int(info["bad"])))
        return j, vol

    def test_empty(self):
        j, _ = self.check("empty")
        self.assertEqual(j.state, "empty")

    def test_few(self):
        self.check("few")

    def test_rings(self):
        j, vol = self.check("rings")
        # Журнал прошёл два круга: в нём от (N-2)·64 до (N-1)·64-1 записей (G1).
        self.assertTrue(510 * 64 <= j.count <= 511 * 64 - 1, j.count)

    def test_seq_wraps(self):
        j, _ = self.check("seqwrap")
        self.assertGreater(j.tail_seq, j.next_seq, "номера перешли через 2^32")

    def test_damaged(self):
        _, vol = self.check("damaged")
        self.assertGreater(vol.n_bad, 64, "сектор нулей и отдельные слоты пропущены")

    def test_restart_not_adjacent(self):
        j, _ = self.check("restart")
        self.assertEqual((j.count, j.tail_seq), (300, 100000), "старые записи в журнал не вошли")

    def test_torn_erase(self):
        self.check("torn_erase")

    def test_import(self):
        _, vol = self.check("import")
        self.assertEqual(len(vol.old), 9000)
        self.assertFalse(vol.import_pending())

    def test_import_pending(self):
        # Том такой журнал не отдаёт (импорт повторится), а разбор образа его
        # показывает, но помечает: --wipe по нему не пойдёт.
        self.assertEqual(self.info["import_pending"]["open"], "2")   # LOG_OPEN_NEED_POWER
        _, vol = self.check("import_pending")
        self.assertTrue(vol.import_pending())

    def test_foreign(self):
        j = log_image.Journal(slurp(self.path("foreign", ".img")))
        self.assertEqual(j.state, "foreign")
        self.assertIsNone(self.want("foreign", ".data.txt"))

    def test_unpack_image(self):
        # unpack_dump.py --image: полный образ — 0 и файлы тома; неполный,
        # не тот хеш, чужая флеш, незаконченный импорт — не 0 (на этом
        # read_data.sh --wipe останавливается).
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        out, old = os.path.join(d, "data.txt"), os.path.join(d, "data_old.txt")
        img = self.path("import", ".img")
        size = str(log_image.FLASH_BYTES)
        h = "%08X" % fnv1a(slurp(img))
        rc, so, se = run_tool("unpack_dump.py", "--image", img, out, old, size, h)
        self.assertEqual(rc, 0, so + se)
        self.assertEqual(slurp(out), self.want("import", ".data.txt"))
        self.assertEqual(slurp(old), self.want("import", ".old.txt"))

        cut = os.path.join(d, "cut.img")
        spit(cut, slurp(img)[:-4096])
        self.assertEqual(run_tool("unpack_dump.py", "--image", cut, out, old, size)[0], 1, "неполный образ")
        self.assertEqual(run_tool("unpack_dump.py", "--image", img, out, old, size, "%08X" % (int(h, 16) ^ 1))[0], 1,
                         "образ не тот, что на флеш")
        self.assertEqual(run_tool("unpack_dump.py", "--image", self.path("foreign", ".img"), out, old, size)[0], 1)
        self.assertEqual(run_tool("unpack_dump.py", "--image", self.path("import_pending", ".img"),
                                  out, old, size)[0], 1)
        # Прошлый data_old.txt не остаётся рядом с новым data.txt без старого архива.
        self.assertEqual(run_tool("unpack_dump.py", "--image", self.path("few", ".img"), out, old, size)[0], 0)
        self.assertFalse(os.path.exists(old))

    def test_summaries_read_image(self):
        # Сводки принимают образ так же, как data.txt с тома.
        img = self.path("import", ".img")
        for name in ("summarize_data.py", "analyze_run.py"):
            rc, out, err = run_tool(name, img)
            self.assertEqual(rc, 0, err)
            self.assertIn("образ журнала", out)
            self.assertIn("500 строк", out)


class UnpackUsb(unittest.TestCase):
    """data.txt с тома USB сверяется с info.txt того же тома: сорванная
    копия не выдаёт себя за свежий короткий архив."""

    INFO = ("FieldSensor build Oct 22 2026 10:00:00\r\n"
            "data.txt: 3 lines, first 2026-10-01 00:00:00, last 2026-10-01 02:00:00\r\n"
            "damaged log slots skipped: 0\r\n"
            "last write_fails=0 lowpower_skips=0 boot_count=1\r\n")

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d)
        self.data = os.path.join(self.d, "data.txt")
        self.info = os.path.join(self.d, "info.txt")
        spit(self.info, self.INFO.encode())

    def run_check(self, rows):
        spit(self.data, ("\r\n".join([NEW_HEAD] + rows) + "\r\n").encode())
        return run_tool("unpack_dump.py", "--usb", self.data, self.info)[0]

    def test_complete(self):
        self.assertEqual(self.run_check([new_row("2026-10-01 %02d:00:00" % h) for h in range(3)]), 0)

    def test_short_copy(self):
        self.assertEqual(self.run_check([new_row("2026-10-01 %02d:00:00" % h) for h in range(2)]), 1)

    def test_middle_line_lost(self):
        # Последняя строка на месте, а в середине строки нет.
        self.assertEqual(self.run_check([new_row("2026-10-01 %02d:00:00" % h) for h in (0, 2)]), 1)

    def test_cut_mid_line(self):
        rows = [new_row("2026-10-01 %02d:00:00" % h) for h in range(3)]
        spit(self.data, ("\r\n".join([NEW_HEAD] + rows) + "\r\n").encode()[:-20])
        self.assertEqual(run_tool("unpack_dump.py", "--usb", self.data, self.info)[0], 1)

    def test_wrong_last(self):
        self.assertEqual(self.run_check([new_row("2026-10-01 %02d:00:00" % h) for h in (0, 1, 3)]), 1)

    def test_not_available(self):
        spit(self.info, b"FieldSensor x\r\nARCHIVE NOT AVAILABLE: flash read error.\r\n")
        self.assertEqual(self.run_check([]), 1)

    def test_fnv_and_check(self):
        spit(self.data, b"abc\r\n")
        out = run_tool("unpack_dump.py", "--fnv", self.data)[1].strip()
        self.assertEqual(int(out, 16), fnv1a(b"abc\r\n"))
        self.assertEqual(run_tool("unpack_dump.py", "--check", self.data, hex(fnv1a(b"abc\r\n")))[0], 0)
        self.assertEqual(run_tool("unpack_dump.py", "--check", self.data, hex(fnv1a(b"abd\r\n")))[0], 1)


class BatteryCurve(unittest.TestCase):
    """D2: заряд — той же кривой, что в прошивке; срок — от текущего заряда."""

    def test_table_points(self):
        self.assertEqual(analyze_run.volts_to_pct(3.80), 65)
        self.assertEqual(analyze_run.volts_to_pct(3.70), 52)
        self.assertEqual(analyze_run.volts_to_pct(4.30), 100)
        self.assertEqual(analyze_run.volts_to_pct(2.90), 0)
        self.assertAlmostEqual(analyze_run.volts_to_pct(3.75), 58.5)

    @unittest.skipUnless(GCC, "нет gcc — сверка с battery_curve.c пропущена")
    def test_matches_firmware(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        lib = os.path.join(d, "curve.dll" if os.name == "nt" else "libcurve.so")
        subprocess.check_call([GCC, "-shared", "-fPIC", "-I", os.path.join(ROOT, "Core", "Inc"),
                               "-o", lib, os.path.join(ROOT, "Core", "Src", "battery_curve.c")])
        f = ctypes.CDLL(lib).Battery_VoltsToPercent
        f.argtypes = [ctypes.c_float]
        f.restype = ctypes.c_uint8
        for mv in range(2900, 4301, 7):
            v = mv / 1000.0
            p = analyze_run.volts_to_pct(v)
            if abs(p - round(p) - 0.5) < 0.02 or abs(p - round(p) + 0.5) < 0.02:
                continue  # граница округления, float против double
            self.assertEqual(int(p + 0.5), f(v), "%.3f В" % v)

    def test_days_from_current_charge(self):
        self.assertAlmostEqual(analyze_run.days_left(-0.5, 60.0), 5.0)
        self.assertIsNone(analyze_run.days_left(0.0, 60.0))

    def test_nan_first_not_104_days(self):
        rc, out, _ = run_tool("analyze_run.py", DATA)
        self.assertEqual(rc, 0)
        m = re.search(r"осталось от текущих (\d+) %: ~(\d+) суток", out)
        self.assertTrue(m, out)
        self.assertEqual(int(m.group(1)), 52)
        self.assertTrue(30 <= int(m.group(2)) <= 36, out)
        self.assertNotIn("~104", out)


class NanRanges(unittest.TestCase):
    """D8: nan в первой строке не превращает диапазон в «nan .. nan»."""

    def check(self, name):
        rc, out, _ = run_tool(name, DATA)
        self.assertEqual(rc, 0)
        line = [l for l in out.splitlines() if l.strip().startswith("air_temp")]
        self.assertEqual(len(line), 1, out)
        self.assertRegex(line[0], r"20\.00 \.\.\s+24\.00\s+nan: 1")

    def test_summarize(self):
        self.check("summarize_data.py")

    def test_analyze(self):
        self.check("analyze_run.py")

    def test_ds18b20_reset_value(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        p = os.path.join(d, "ds85.txt")
        s = slurp(DATA).replace(b",18.00,40.00,60,3.75", b",85.00,40.00,60,3.75")
        spit(p, s)
        for name in ("summarize_data.py", "analyze_run.py"):
            out = run_tool(name, p)[1]
            line = [l for l in out.splitlines() if l.strip().startswith("soil_temp ")]
            self.assertTrue(line, out)
            self.assertRegex(line[-1], r"18\.00 \.\.\s+18\.00\s+85\.00 \(сброс DS18B20\): \d+")


class DsErrCodes(unittest.TestCase):
    """R9: коды ds_err 5/6/7 расшифрованы в сводке, а не «ds_err != 0»."""

    def test_codes_named(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        p = os.path.join(d, "ds5.txt")
        lines = slurp(DATA).split(b"\r\n")
        for i, code in ((1, b"5"), (2, b"5"), (3, b"6"), (4, b"7")):
            lines[i] = lines[i][:lines[i].rfind(b",", 0, lines[i].rfind(b","))] + b"," + code + b",0"
        spit(p, b"\r\n".join(lines))
        for name in ("summarize_data.py", "analyze_run.py"):
            out = run_tool(name, p)[1]
            self.assertIn("ds_err != 0", out)
            self.assertIn("код 5 (служебные байты scratchpad не те", out)
            self.assertRegex(out, r"код 5 \([^)]*\) x2")
            self.assertIn("код 6 (85.00 °C по сбросу", out)
            self.assertIn("код 7 (вне шкалы", out)


def main_c():
    return src_text("Core", "Src", "main.c")


def firmware_header():
    # Набор колонок живёт в log_record.h (LOG_CSV_HEADER); CSV_HEADER в main.c
    # — его синоним.
    m = re.search(r'#define LOG_CSV_HEADER((?:\s*"[^"]*"\s*\\?)+)', src_text("Core", "Inc", "log_record.h"))
    return "".join(re.findall(r'"([^"]*)"', m.group(1))).split(",")


class HeaderColumns(unittest.TestCase):
    """Заголовок прошивки, формат строки и разбор — одно и то же (П4)."""

    def test_format_matches_header(self):
        # Число полей в snprintf строки CSV (LogRecord_FormatMeas) — ровно
        # число колонок заголовка.
        src = src_text("Core", "Inc", "log_record.h")
        m = re.search(r'snprintf\(out, size,\s*((?:"[^"]*"\s*)+),', src)
        self.assertTrue(m, "не нашёл snprintf строки CSV в log_record.h")
        fmt = "".join(re.findall(r'"([^"]*)"', m.group(1)))
        convs = re.findall(r"%[-+ #0]*\d*(?:\.\d+)?(?:l|ll|h)?[sdufxX]", fmt)
        self.assertEqual(len(convs), len(firmware_header()), fmt)

    def test_main_uses_log_header(self):
        self.assertRegex(main_c(), r"#define CSV_HEADER LOG_CSV_HEADER\b")

    def test_new_columns_fixture_is_current(self):
        self.assertEqual(NEW_HEAD.split(","), firmware_header(), "образец NewColumns устарел")

    def test_parser_knows_columns(self):
        import summarize_data
        head = set(firmware_header())
        legacy = {"aht_i2c_err"}   # колонка старых архивов
        for n in summarize_data.ERROR_COLS + summarize_data.FLAG_COLS + tuple(summarize_data.CODE_TEXT):
            if n not in legacy:
                self.assertIn(n, head, "разбор ждёт колонку, которой в прошивке нет")
        for n in ("reset_cause", "boot_count", "time_ok", "soil_raw", "bat_raw", "vdd"):
            self.assertIn(n, head)


NEW_HEAD = ("timestamp,air_temp,air_hum,soil_temp,soil_hum,bat_pct,vbat,vdd,soil_valid,lse_ok,"
            "time_ok,aht_init_st,aht_read_st,ds_err,air_xchk,i2c_rec,sens_off,write_fails,"
            "lowpower_skips,reset_cause,boot_count,soil_raw,bat_raw")


def new_row(ts, time_ok=1, ds_err=0, xchk=0, i2c=0, sens=0, cause=7, boot=1, air="21.00"):
    return ("%s,%s,40.00,18.00,35.00,60,3.75,3.300,1,1,%d,0,0,%d,%d,%d,%d,0,0,%d,%d,2048,2300"
            % (ts, air, time_ok, ds_err, xchk, i2c, sens, cause, boot))


class NewColumns(unittest.TestCase):
    """Разбор архива с колонками П4: недоверенные метки, коды, старты."""

    def setUp(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        self.path = os.path.join(d, "new.txt")
        rows = [new_row("2026-10-01 %02d:00:00" % h) for h in range(10)]
        rows[3] = new_row("2026-10-01 03:00:00", xchk=1)
        rows[4] = new_row("2026-10-01 04:00:00", xchk=2, i2c=2)
        # Замена банки: календарь потерян, часы встали на время сборки (раньше),
        # прибор перезапустился от питания; метки этих строк не в счёт.
        rows[5] = new_row("2026-09-01 00:00:00", time_ok=0, boot=1, cause=7)
        rows[6] = new_row("2026-09-01 01:00:00", time_ok=0, boot=1, cause=7)
        # Затем — HardFault и программный перезапуск (boot_count 2).
        # Программный сброс на F401 взводит и PINRSTF — как в поле.
        hf = 8 | 2 | (1 << 8)
        rows[7] = new_row("2026-10-01 07:00:00", boot=2, cause=hf)
        rows[8] = new_row("2026-10-01 08:00:00", boot=2, cause=hf, ds_err=8, sens=5, air="nan")
        rows[9] = new_row("2026-10-01 09:00:00", boot=2, cause=hf)
        spit(self.path, ("\r\n".join([NEW_HEAD] + rows) + "\r\n").encode())

    def test_summarize(self):
        rc, out, err = run_tool("summarize_data.py", self.path)
        self.assertEqual(rc, 0, err)
        self.assertIn("меткам 2 строк не верить (time_ok = 0)", out)
        self.assertNotIn("время шло назад", out, "откат часов по замене банки — не перепрошивка")
        self.assertIn("time_ok == 0 в 2 строках", out)
        self.assertIn("код 1 (AHT20 и BMP280 разошлись", out)
        self.assertIn("код 2 (BMP280 не ответил", out)
        self.assertIn("код 2 (шину освободило только снятие питания", out)
        self.assertIn("код 8 (не опрашивался — карантин", out)
        self.assertIn("sens_off != 0 в 1 строках", out)
        # Два старта от питания подряд (оба boot_count = 1) различаются по
        # откату меток: включение, замена банки, HardFault — три старта.
        self.assertIn("стартов в архиве: 3", out)
        self.assertIn("программный, HardFault x1", out)
        self.assertNotIn("NRST", out)
        self.assertIn("питание x2", out)
        self.assertNotIn("reset_cause ", out, "код причины — не диапазон")

    def test_analyze(self):
        rc, out, err = run_tool("analyze_run.py", self.path)
        self.assertEqual(rc, 0, err)
        self.assertIn("строк с time_ok = 0 (часы откатились на время сборки): 2", out)
        self.assertNotIn("время шло назад", out)
        self.assertIn("стартов: 3", out)
        self.assertIn("код 1 (AHT20 и BMP280 разошлись", out)
        self.assertIn("10 строк", out)
        self.assertIn("в 2 из 10", out, "отказы считаются и по недоверенным строкам")

    def test_analyze_all_untrusted(self):
        # После замены банки time_ok = 0 держится до перепрошивки: такой архив
        # не должен превращаться в «нет строк» — отказы и разряд остаются.
        rows = [new_row("2026-09-01 %02d:00:00" % h, time_ok=0, xchk=1 if h == 3 else 0)
                for h in range(10)]
        spit(self.path, ("\r\n".join([NEW_HEAD] + rows) + "\r\n").encode())
        rc, out, err = run_tool("analyze_run.py", self.path)
        self.assertEqual(rc, 0, err)
        self.assertIn("ни одной с достоверной меткой", out)
        self.assertIn("air_xchk != 0 в 1 из 10", out)
        self.assertIn("разряд — по недоверенным меткам", out)

    def test_old_archive_still_parsed(self):
        # Старый архив (data_N.txt после смены заголовка) разбирается как раньше.
        rc, out, _ = run_tool("summarize_data.py", DATA)
        self.assertEqual(rc, 0)
        self.assertNotIn("стартов", out)


# --- Стенд read_data.sh: фейковый программатор вместо платы ----------------
#
# Программатор — Python-эмуляция: «прошивка» — это main.c, каким его собрал
# заглушечный dev.sh; если в нём есть вставка дампера, при прошивке дампер
# «выполняется» над образом флеш board.img (хеш всей флеш, стирание — по тем
# же условиям, что в C-вставке, с размером и хешем, вписанными в текст
# патча), а -w32 в raw_req отдаёт кусок, как dump_raw_step.
# Все python-вызовы идут через обёртку, которая, как Python на Windows,
# пишет в stdout CRLF (R1).

FAKE_PROG = r"""
import os, re, sys
st = os.environ["FAKE_STATE"]
root = os.environ["FAKE_ROOT"]
a = sys.argv[1:]
def out(*lines):
    sys.stdout.write("".join(l + "\r\n" for l in lines))
def fnv(d):
    h = 2166136261
    for b in bytearray(d):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h
board = os.path.join(st, "board.img")
infop = os.path.join(st, "info")
bufp = os.path.join(st, "buf")
def load():
    return [int(x, 16) for x in open(infop).read().split()]
def save(w):
    open(infop, "w").write(" ".join("%08X" % x for x in w))
if a[:2] == ["-l", "st"]:
    out("ST-Link SN : 0001")
elif "-w" in a:
    fw = open(os.path.join(root, "Debug", "built.c"), "rb").read()
    open(os.path.join(st, "fw.c"), "wb").write(fw)
    n = len([f for f in os.listdir(st) if f.startswith("flash")])
    open(os.path.join(st, "flash%d.c" % n), "wb").write(fw)
    if os.environ.get("FAKE_EDIT") and n == 0:
        # Владелец сохранил main.c, пока скрипт ждал окно.
        open(os.path.join(root, "Core", "Src", "main.c"), "ab").write(os.environ["FAKE_EDIT"].encode())
    if b"dump_info" in fw:
        data = open(board, "rb").read()
        m = re.search(rb"dump_wipe\((\d+)U, 0x([0-9A-F]+)U\)", fw)
        if m and os.environ.get("FAKE_CHANGE"):
            # Между проходами плата успела что-то записать.
            data = data[:100] + bytes([data[100] ^ 1]) + data[101:]
            open(board, "wb").write(data)
        w = [0] * 14
        w[0], w[1], w[6], w[7] = 0xD00DFEED, len(data), fnv(data), 1
        if m:
            if int(m.group(1)) == len(data) and int(m.group(2), 16) == w[6]:
                open(board, "wb").write(b"\xff" * len(data))
                w[8] = 1
            else:
                w[8] = 2
        w[2] = 0xFFFFFFFF
        if os.environ.get("FAKE_BAT_FAIL"):
            w[3] = w[4] = w[5] = 0xFFFFFFFF
        else:
            w[3], w[4], w[5] = 3800, 3300, 65
        save(w)
    elif os.path.exists(infop):
        os.remove(infop)
    out("Download verified successfully")
elif "-w32" in a or "-r32" in a:
    if not os.path.exists(infop):
        out("Error: target not halted")
        sys.exit(1)
    w = load()
    if "-w32" in a:
        i = a.index("-w32")
        w[(int(a[i + 1], 16) - 0x20000000) // 4] = int(a[i + 2], 0)
        req = w[9]
        if req and req != w[10]:
            data = open(board, "rb").read()
            off = (req - 1) * 32768
            chunk = data[off:off + 32768]
            if os.environ.get("FAKE_BAD_CHUNK") == str(req):
                chunk = b""
            flaky = os.path.join(st, "flaky%d" % req)
            if os.environ.get("FAKE_FLAKY_CHUNK") == str(req) and not os.path.exists(flaky):
                open(flaky, "w").close()   # разовый сбой чтения флеш
                chunk = b""
            open(bufp, "wb").write(chunk)
            w[11], w[12], w[13], w[10] = off, len(chunk), fnv(chunk), req
        save(w)
        out("Memory write OK")
    else:
        for i in range(0, len(w), 4):
            out("0x%08X : %s" % (0x20000000 + 4 * i, " ".join("%08X" % x for x in w[i:i + 4])))
elif "-r" in a:
    i = a.index("-r")
    n, dst = int(a[i + 2]), a[i + 3]
    d = open(bufp, "rb").read()
    open(dst, "wb").write((d + b"\0" * n)[:n])
    out("Data read successfully")
else:
    out("Error: unknown command")
"""

FAKE_DEV = r"""#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ "$1" = build ] || exit 2
grep -q BUILD_FAIL "$ROOT/Core/Src/main.c" && { echo "error: BUILD_FAIL" >&2; exit 1; }
cp "$ROOT/Core/Src/main.c" "$ROOT/Debug/built.c"
touch "$ROOT/Debug/FieldSensor.elf"
"""

# Обёртка python, печатающая CRLF, как Python на Windows. pipefail сохраняет
# код возврата самого Python.
FAKE_PY = r"""#!/usr/bin/env bash
set -o pipefail
"%s" "$@" | sed 's/$/\r/'
"""

BASH = shutil.which("bash")

# Флеш стенда — 16 секторов (64 КБ, два куска): журнал на 150 записей,
# записанный тем же кодом, что в log_oracle.c, но собранный здесь, без gcc.
BENCH_SECTORS = 16


def bench_record(seq, kind, payload):
    rec = bytes([kind, log_image.VERSION]) + struct.pack("<I", seq) + bytes(payload)
    return rec + struct.pack("<I", log_image.crc32(rec))


def bench_import_pending(sectors=BENCH_SECTORS):
    """Импорт старого архива начат и не закончен: FILE и TEXT без END."""
    img = bytearray(b"\xff" * (sectors * log_image.SECTOR_SIZE))
    p = bytearray(log_image.PAYLOAD_SIZE)
    p[5:15] = b"data_1.txt"
    img[0:64] = bench_record(0, log_image.TYPE_IMPORT_FILE, p)
    for i in range(1, 6):
        p = bytearray(log_image.PAYLOAD_SIZE)
        p[1] = 10
        p[2:12] = b"old text\r\n"
        img[i * 64:(i + 1) * 64] = bench_record(i, log_image.TYPE_TEXT, p)
    return bytes(img)


def bench_journal(n=150, sectors=BENCH_SECTORS):
    """Образ журнала: n замеров подряд с сектора 0, как пишет LogRing_Append
    на чистой флеш."""
    img = bytearray(b"\xff" * (sectors * log_image.SECTOR_SIZE))
    for i in range(n):
        p = bytearray(log_image.PAYLOAD_SIZE)
        p[0:6] = bytes([26, 10, 1 + i // 24, i % 24, 0, 0])
        for k, v in enumerate((21.0 + i % 3, 40.0, 18.0, 35.0, 3.75 - i * 0.001, 3.3)):
            p[6 + 4 * k:10 + 4 * k] = struct.pack("<f", v)
        p[30] = 60
        p[31:35] = struct.pack("<HH", 2048, 2300)
        p[35:37] = struct.pack("<H", 0b1101)   # bat_valid, lse_ok, time_ok
        p[49:53] = struct.pack("<I", 1)
        rec = bytes([log_image.TYPE_MEAS, log_image.VERSION]) + struct.pack("<I", i) + bytes(p)
        rec += struct.pack("<I", log_image.crc32(rec))
        img[i * 64:(i + 1) * 64] = rec
    return bytes(img)


@unittest.skipUnless(BASH and os.name != "nt" and shutil.which("git") and shutil.which("sed"),
                     "стенд read_data.sh гоняется только под bash вне Windows")
class ReadDataBench(unittest.TestCase):
    """read_data.sh целиком: копирование с тома USB и снятие по SWD (R1, R4–R6, --wipe)."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        r = self.root
        for d in ("tools", "Core/Src", "Debug", "bin", "state", "vol"):
            os.makedirs(os.path.join(r, d))
        for f in ("read_data.sh", "cubeide.sh", "dump_patch.py", "unpack_dump.py", "summarize_data.py",
                  "log_image.py"):
            shutil.copy(os.path.join(TOOLS, f), os.path.join(r, "tools", f))
        self.orig = slurp(os.path.join(ROOT, "Core", "Src", "main.c"))
        spit(os.path.join(r, "Core", "Src", "main.c"), self.orig)
        spit(os.path.join(r, "Debug", "FieldSensor.map"),
             b" .bss.dump_info  0x20000000       0x38 ./Core/Src/main.o\n"
             b" .bss.dump_buf   0x20000100     0x8000 ./Core/Src/main.o\n")
        self.state = os.path.join(r, "state")
        self.flash = bench_journal()
        self.board(self.flash)
        vol = log_image.Volume(log_image.Journal(self.flash))
        self.data = vol.data
        self.vol = os.path.join(r, "vol")
        execs = {
            "tools/dev.sh": FAKE_DEV,
            "bin/python": FAKE_PY % sys.executable,
            "bin/cygpath": '#!/bin/sh\n[ "$1" = -w ] && shift\necho "$1"\n',
            "bin/STM32_Programmer_CLI.exe": "#!%s\n%s" % (sys.executable, FAKE_PROG),
        }
        for name, text in execs.items():
            p = os.path.join(r, name)
            spit(p, text.encode("utf-8"))
            os.chmod(p, 0o755)

    def board(self, data):
        spit(os.path.join(self.state, "board.img"), data)

    def board_now(self):
        return slurp(os.path.join(self.state, "board.img"))

    def run_sh(self, *args, **env):
        e = dict(os.environ)
        e.update(PATH=os.path.join(self.root, "bin") + os.pathsep + e["PATH"],
                 STM32_PROG=os.path.join(self.root, "bin", "STM32_Programmer_CLI.exe"),
                 FAKE_STATE=self.state, FAKE_ROOT=self.root, FLASH_WAIT_SEC="5",
                 FS_VOLUME=self.vol, USB_WAIT_SEC="0", DUMP_WAIT_TRIES="2")
        e.update(env)
        p = subprocess.run([BASH, os.path.join(self.root, "tools", "read_data.sh")] + list(args),
                           cwd=self.root, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def build(self, name):
        return os.path.join(self.root, "build", name)

    def archive(self):
        d = self.build("archive")
        return [os.path.join(d, f) for f in sorted(os.listdir(d))] if os.path.isdir(d) else []

    def flashed(self):
        return sorted(f for f in os.listdir(self.state) if f.startswith("flash"))

    # ---- USB

    def usb_volume(self, data=None, info=None, old=None):
        lines = self.data.count(b"\r\n") - 1
        spit(os.path.join(self.vol, "info.txt"), info or (
            "FieldSensor build test\r\ndata.txt: %d lines, first 2026-10-01 00:00:00, last 2026-10-07 05:00:00\r\n"
            "damaged log slots skipped: 0\r\n" % lines).encode())
        spit(os.path.join(self.vol, "data.txt"), self.data if data is None else data)
        if old:
            spit(os.path.join(self.vol, "data_old.txt"), old)

    def test_usb_copy(self):
        # Путь по умолчанию: копия с тома, сверка с info.txt, сводка. Ни
        # программатора, ни дампера.
        self.usb_volume(old=b"old archive\r\n")
        rc, out, err = self.run_sh()
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(slurp(self.build("data.txt")), self.data)
        self.assertEqual(slurp(self.build("data_old.txt")), b"old archive\r\n")
        self.assertIn("сверено с info.txt: 150 строк", out)
        self.assertIn("150 строк, 23 колонок", out)
        self.assertEqual(self.flashed(), [])

    def test_usb_short_copy_rejected(self):
        self.usb_volume()
        self.assertEqual(self.run_sh()[0], 0)
        self.usb_volume(data=self.data[:-300])
        rc, out, err = self.run_sh()
        self.assertEqual(rc, 1, out + err)
        self.assertFalse(os.path.exists(self.build("data.txt")), "сорванная копия не легла на место архива")
        self.assertEqual(slurp(self.build("data.txt.prev")), self.data)

    def test_usb_archive_not_available(self):
        self.usb_volume(info=b"FieldSensor x\r\nARCHIVE NOT AVAILABLE: flash read error.\r\n")
        os.remove(os.path.join(self.vol, "data.txt"))
        rc, out, err = self.run_sh()
        self.assertEqual(rc, 1, out + err)
        self.assertIn("ARCHIVE NOT AVAILABLE", err)

    def test_usb_found_among_drives(self):
        # Без FS_VOLUME том ищется перебором корней (на Windows — буквы
        # дисков); чужой диск с другим info.txt пропускается.
        self.usb_volume()
        other = os.path.join(self.root, "other")
        os.makedirs(other)
        spit(os.path.join(other, "info.txt"), b"not a sensor\r\n")
        rc, out, err = self.run_sh(FS_VOLUME="", FS_ROOTS="%s %s /nonexistent" % (other, self.vol))
        self.assertEqual(rc, 0, out + err)
        self.assertIn("том: " + self.vol, out)

    def test_usb_default_roots_are_drive_letters(self):
        # R7: маска /[d-z] в Git Bash не раскрывается — буквы перебираются явно.
        sh = slurp(os.path.join(TOOLS, "read_data.sh")).decode("utf-8")
        self.assertIn("for drv in d e f g h i j k l m n o p q r s t u v w x y z", sh)
        self.assertNotRegex(sh, r"for d in /\[")

    def test_usb_no_volume(self):
        rc, out, err = self.run_sh()
        self.assertEqual(rc, 2, out + err)
        self.assertIn("--swd", err)

    # ---- SWD

    def test_swd_read(self):
        rc, out, err = self.run_sh("--swd")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(slurp(self.build("data.img")), self.flash)
        self.assertEqual(slurp(self.build("data.txt")), self.data)
        self.assertEqual(self.board_now(), self.flash, "только чтение")
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)
        self.assertEqual(len(self.flashed()), 2, "дампер и рабочая версия")

    def test_swd_chunk_retried(self):
        # Разовый сбой чтения куска: повтор того же номера (через сброс
        # raw_done) снимает его, образ полный.
        rc, out, err = self.run_sh("--swd", FAKE_FLAKY_CHUNK="2")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(slurp(self.build("data.img")), self.flash)

    def test_wipe_survives_crlf(self):
        # R1: хеш с диска сверяется не сравнением строк с CR на конце.
        rc, out, err = self.run_sh("--wipe")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(self.board_now(), b"\xff" * len(self.flash), "флеш не стёрта")
        self.assertNotIn("не совпал", err)
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)

    def test_wiped_archive_kept(self):
        # R5: копия, после которой плата стёрта, переживает следующие снятия.
        self.assertEqual(self.run_sh("--wipe")[0], 0)
        arc = self.archive()
        self.assertEqual(len(arc), 2)
        img = [f for f in arc if f.endswith(".img")][0]
        csv = [f for f in arc if f.endswith(".txt")][0]
        self.assertEqual(slurp(img), self.flash)
        self.assertEqual(slurp(csv), self.data)
        self.assertRegex(os.path.basename(img), r"^flash-\d{8}-\d{6}-[0-9A-F]{8}\.img$")
        for n in (10, 20):
            self.board(bench_journal(n))
            self.assertEqual(self.run_sh("--swd")[0], 0)
        self.assertEqual(self.archive(), arc)
        self.assertEqual(slurp(img), self.flash)

    def test_wipe_refused_on_incomplete_image(self):
        rc, out, err = self.run_sh("--wipe", FAKE_BAD_CHUNK="2")
        self.assertEqual(rc, 1, out + err)
        self.assertIn("образ неполный", err)
        self.assertEqual(self.board_now(), self.flash)
        self.assertEqual(len(self.flashed()), 2, "второй дампер не прошивался")
        self.assertEqual(self.archive(), [])

    def test_wipe_refused_when_board_changed(self):
        rc, out, err = self.run_sh("--wipe", FAKE_CHANGE="1")
        self.assertEqual(rc, 3, out + err)
        self.assertIn("флеш уже не та", err)
        self.assertNotEqual(self.board_now(), b"\xff" * len(self.flash))
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)

    def test_wipe_refused_without_journal(self):
        # Чужая флеш (старый том, ещё не перенесённый): data.txt не собрать,
        # стирать нечем подтверждённое.
        self.board(b"timestamp,a\r\n2026-01-01 00:00:00,1\r\n".ljust(BENCH_SECTORS * 4096, b"\0"))
        rc, out, err = self.run_sh("--wipe")
        self.assertEqual(rc, 1, out + err)
        self.assertNotEqual(self.board_now()[:4], b"\xff" * 4)

    def test_wipe_refused_on_pending_import(self):
        # Импорт старого архива не закончен: старый том цел только на флеш,
        # в журнале его текст не весь. Разбор — не 0, второй дампер не
        # прошивается, флеш не тронута.
        img = bench_import_pending()
        self.board(img)
        rc, out, err = self.run_sh("--wipe")
        self.assertEqual(rc, 1, out + err)
        self.assertEqual(self.board_now(), img)
        self.assertEqual(len(self.flashed()), 2, "второй дампер не прошивался")
        self.assertEqual(self.archive(), [])

    def test_foreign_edit_not_flashed(self):
        # R4: правка main.c во время снятия (здесь — даже не собирающаяся)
        # не уходит на плату; рабочая прошивка — main.c до снятия байт-в-байт,
        # правка возвращается в main.c после прошивки и лежит копией рядом.
        edit = "\n/* BUILD_FAIL: недописано */\n"
        rc, out, err = self.run_sh("--swd", FAKE_EDIT=edit)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)
        self.assertIn("ИЗМЕНИЛИ", err)
        self.assertEqual(slurp(os.path.join(self.root, "Core", "Src", "main.c")),
                         self.orig + edit.encode("utf-8"))
        yours = [f for f in os.listdir(self.build("")) if f.startswith("main.c.yours-")]
        self.assertEqual(len(yours), 1)
        self.assertEqual(slurp(self.build(yours[0])), self.orig + edit.encode("utf-8"))

    def test_battery_fail_not_empty_bank(self):
        # R6: отказ замера батареи — не «0 мВ (255%)».
        rc, out, err = self.run_sh("--swd", FAKE_BAT_FAIL="1")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("батарея — замер не удался", out)
        rc, out, err = self.run_sh("--swd")
        self.assertIn("батарея 3800 мВ (65%), VDD 3300 мВ", out)


if __name__ == "__main__":
    unittest.main()
