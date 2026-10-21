# -*- coding: utf-8 -*-
"""Самопроверка инструментов: плата не нужна.

    python tools/test_tools.py

Проверяется то, на чём инструменты уже ошибались (находки D1, D2, D3, D5, D8
аудита и R1, R4–R6, R9 приёмки): вставка дампера не меняет окончания строк и снимается без следа;
дампер стирает архив только тот, что хост сохранил целиком; срок батареи
считается по кривой прошивки от текущего заряда; nan не портит диапазоны;
испорченное чтение ОЗУ не попадает на диск.

Проверка самой C-вставки дампера собирает её хостовым gcc с заглушками FatFs.
Где gcc нет (у владельца тесты идут под MSVC), эти проверки пропускаются.
"""
import ctypes
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import analyze_run  # noqa: E402
import dump_patch   # noqa: E402

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
        s = io.open(os.path.join(ROOT, "Core", "Src", "main.c"),
                    encoding="utf-8", newline="").read().replace("\r\n", "\n")
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


# Заглушки ровно под то, что использует вставка дампера.
STUBS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4 } FRESULT;
typedef struct { uint32_t pos; } FIL;
#define FA_READ 1
#define DATA_FILE "data.txt"
static uint8_t fs_ok = 1;
static uint32_t write_fails = 0;
static uint8_t g_file[200000];
static uint32_t g_size;
static int g_exists = 1, g_read_fail = -1, g_fail_full = 0, g_reads = 0, g_unlinked = 0;
static FRESULT f_open(FIL *f, const char *n, int m)
{ (void)n; (void)m; if (!g_exists) return FR_NO_FILE; f->pos = 0; return FR_OK; }
#define f_size(f) (g_size)
static FRESULT f_read(FIL *f, void *b, UINT n, UINT *br)
{
    int fail = (g_reads++ == g_read_fail);
    uint32_t left = g_size - f->pos; if (n > left) n = left;
    if (fail && !g_fail_full) { *br = 0; return FR_DISK_ERR; }
    /* g_fail_full: байты на месте, но драйвер вернул ошибку — верить им нельзя */
    memcpy(b, g_file + f->pos, n); f->pos += n; *br = n;
    return fail ? FR_DISK_ERR : FR_OK;
}
static FRESULT f_lseek(FIL *f, uint32_t p) { f->pos = p; return FR_OK; }
static FRESULT f_close(FIL *f) { (void)f; return FR_OK; }
static FRESULT f_unlink(const char *n) { (void)n; g_unlinked = 1; g_exists = 0; return FR_OK; }
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
"""

MAIN = r"""
int main(int argc, char **argv)
{
    g_size = (uint32_t)atoi(argv[1]);
    g_exists = atoi(argv[2]);
    fs_ok = (uint8_t)atoi(argv[3]);
    g_read_fail = atoi(argv[4]);
    g_fail_full = atoi(argv[5]);
    g_bat_valid = atoi(argv[6]);
    for (uint32_t i = 0; i < g_size; i++) g_file[i] = (uint8_t)('a' + i % 26);
    run_dump();
    printf("%u %u %u %u %u %d %u %u %u\n", (unsigned)dump_info.len, (unsigned)dump_info.win_bytes,
           (unsigned)dump_info.status, (unsigned)dump_info.hash, (unsigned)dump_info.wipe, g_unlinked,
           (unsigned)dump_info.bat_mv, (unsigned)dump_info.vdd_mv, (unsigned)dump_info.bat_pct);
    return 0;
}
"""


def file_bytes(size):
    return bytes(ord("a") + i % 26 for i in range(size))


def windowed(size):
    d = file_bytes(size)
    step = (size - 1024) // 31
    return b"".join(d[i * step:i * step + 1024] for i in range(32))


@unittest.skipUnless(GCC, "нет gcc — проверка C-вставки дампера пропущена")
class DumperWipe(unittest.TestCase):
    """D1: дампер удаляет архив, только если хост сохранил его целиком и тот
    совпал по размеру и хешу. Собирается ровно тот текст, что вставляет патч."""

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
        dump = dump.replace("while (1) { __NOP(); }", "return;")
        src = STUBS + decl + "\nstatic void run_dump(void)\n{\n  " + dump + "\n}\n" + MAIN
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t" + EXE)
        spit(c, src.encode("utf-8"))
        subprocess.check_call([GCC, "-std=c11", "-Wall", "-o", exe, c])
        return exe

    def run_exe(self, exe, size, exists=1, fs=1, read_fail=-1, fail_full=0, bat=1):
        out = subprocess.check_output([exe, str(size), str(exists), str(fs),
                                       str(read_fail), str(fail_full), str(bat)])
        ln, win, st, h, wipe, unlinked, bmv, vmv, pct = map(int, out.split())
        return dict(len=ln, win=win, status=st, hash=h, wipe=wipe, unlinked=unlinked,
                    bat=(bmv, vmv, pct))

    def test_read_only_never_unlinks(self):
        r = self.run_exe(self.build(), 1000)
        self.assertEqual((r["len"], r["wipe"], r["unlinked"]), (1000, 0, 0))
        self.assertEqual(r["hash"], fnv1a(file_bytes(1000)))

    def test_wipe_when_matches(self):
        exe = self.build("--wipe", "1000", hex(fnv1a(file_bytes(1000))))
        r = self.run_exe(exe, 1000)
        self.assertEqual((r["wipe"], r["unlinked"]), (1, 1))

    def test_wipe_refused_on_read_error(self):
        exe = self.build("--wipe", "1000", hex(fnv1a(b"")))
        r = self.run_exe(exe, 1000, read_fail=0)
        self.assertEqual(r["status"] & 0xFF, 3)
        self.assertEqual((r["wipe"], r["unlinked"]), (2, 0))

    def test_wipe_refused_on_read_error_full_buffer(self):
        # Буфер полон и хеш сходится, но f_read вернул ошибку: отказ только по статусу.
        exe = self.build("--wipe", "1000", hex(fnv1a(file_bytes(1000))))
        r = self.run_exe(exe, 1000, read_fail=0, fail_full=1)
        self.assertEqual(r["status"], 3 | (1 << 8))
        self.assertEqual((r["wipe"], r["unlinked"]), (2, 0))

    def test_wipe_refused_on_size_change(self):
        # Хеш совпадает, размер нет: отказ только по размеру.
        exe = self.build("--wipe", "1000", hex(fnv1a(file_bytes(1065))))
        r = self.run_exe(exe, 1065)
        self.assertEqual((r["wipe"], r["unlinked"]), (2, 0))

    def test_wipe_refused_on_hash_mismatch(self):
        exe = self.build("--wipe", "1000", "0x12345678")
        r = self.run_exe(exe, 1000)
        self.assertEqual((r["wipe"], r["unlinked"]), (2, 0))

    def test_wipe_refused_when_windowed(self):
        # Размер и хеш совпадают с тем, что лежит в буфере, — отказ только
        # потому, что в буфер попали окна, а не весь файл.
        size = 100000
        exe = self.build("--wipe", str(size), hex(fnv1a(windowed(size))))
        r = self.run_exe(exe, size)
        self.assertEqual(r["win"], 1024)
        self.assertEqual(r["hash"], fnv1a(windowed(size)))
        self.assertEqual((r["wipe"], r["unlinked"]), (2, 0))

    def test_battery_fail_marked(self):
        # R6: отказ замера батареи — 0xFFFFFFFF, а не 0 мВ и 255 %.
        exe = self.build()
        self.assertEqual(self.run_exe(exe, 10)["bat"], (3800, 3300, 65))
        self.assertEqual(self.run_exe(exe, 10, bat=0)["bat"], (0xFFFFFFFF,) * 3)

    def test_status_no_fs_and_no_file(self):
        exe = self.build()
        self.assertEqual(self.run_exe(exe, 1000, fs=0)["status"], 1)
        self.assertEqual(self.run_exe(exe, 1000, exists=0)["status"], 2 | (4 << 8))


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


class UnpackHash(unittest.TestCase):
    """D1/D3: прочитанное по SWD, не совпавшее с буфером дампера, на диск не идёт."""

    def test_mismatch_writes_nothing(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        src, dst = os.path.join(d, "in.bin"), os.path.join(d, "out.csv")
        spit(src, b"abc\r\n")
        rc = run_tool("unpack_dump.py", src, dst, "0", "0", hex(fnv1a(b"abd\r\n")))[0]
        self.assertNotEqual(rc, 0)
        self.assertFalse(os.path.exists(dst))
        rc = run_tool("unpack_dump.py", src, dst, "0", "0", hex(fnv1a(b"abc\r\n")))[0]
        self.assertEqual(rc, 0)
        self.assertEqual(slurp(dst), b"abc\r\n")
        out = run_tool("unpack_dump.py", "--fnv", dst)[1].strip()
        self.assertEqual(int(out, 16), fnv1a(b"abc\r\n"))


# --- Стенд read_data.sh: фейковый программатор вместо платы ----------------
#
# Программатор — Python-эмуляция: «прошивка» — это main.c, каким его собрал
# заглушечный dev.sh; если в нём есть вставка дампера, при прошивке дампер
# «выполняется» над файлом board.txt (стирание — по тем же условиям, что
# в C-вставке, с размером и хешем, вписанными в текст патча).
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
board = os.path.join(st, "board.txt")
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
    info = None
    if b"dump_info" in fw:
        data = open(board, "rb").read() if os.path.exists(board) else None
        w = [0xD00DFEED] + [0] * 12
        if data is None:
            w[10] = 2 | (4 << 8)
            data = b""
        else:
            w[1] = w[2] = len(data)
            w[11] = fnv(data)
            m = re.search(rb"dump_info.fsize == (\d+)U && dump_info.hash == 0x([0-9A-F]+)U", fw)
            if m:
                if int(m.group(1)) == len(data) and int(m.group(2), 16) == w[11]:
                    os.remove(board)
                    w[12] = 1
                else:
                    w[12] = 2
        w[6] = 0xFFFFFFFF
        if os.environ.get("FAKE_BAT_FAIL"):
            w[7] = w[8] = w[9] = 0xFFFFFFFF
        else:
            w[7], w[8], w[9] = 3800, 3300, 65
        open(os.path.join(st, "info"), "w").write(" ".join("%08X" % x for x in w))
        open(os.path.join(st, "buf"), "wb").write(data)
    elif os.path.exists(os.path.join(st, "info")):
        os.remove(os.path.join(st, "info"))
    out("Download verified successfully")
elif "-r32" in a:
    p = os.path.join(st, "info")
    if not os.path.exists(p):
        out("Error: target not halted")
    else:
        w = open(p).read().split()
        for i in range(0, len(w), 4):
            out("0x%08X : %s" % (0x20000000 + 4 * i, " ".join(w[i:i + 4])))
elif "-r" in a:
    i = a.index("-r")
    n, dst = int(a[i + 2]), a[i + 3]
    d = open(os.path.join(st, "buf"), "rb").read()
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


@unittest.skipUnless(BASH and os.name != "nt" and shutil.which("git") and shutil.which("sed"),
                     "стенд read_data.sh гоняется только под bash вне Windows")
class ReadDataBench(unittest.TestCase):
    """R1, R4, R5, R6: read_data.sh целиком, с CRLF от Python."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        r = self.root
        for d in ("tools", "Core/Src", "Debug", "bin", "state"):
            os.makedirs(os.path.join(r, d))
        for f in ("read_data.sh", "cubeide.sh", "dump_patch.py", "unpack_dump.py", "summarize_data.py"):
            shutil.copy(os.path.join(TOOLS, f), os.path.join(r, "tools", f))
        self.orig = slurp(os.path.join(ROOT, "Core", "Src", "main.c"))
        spit(os.path.join(r, "Core", "Src", "main.c"), self.orig)
        spit(os.path.join(r, "Debug", "FieldSensor.map"),
             b" .bss.dump_info  0x20000000       0x34 ./Core/Src/main.o\n"
             b" .bss.dump_buf   0x20000100     0x8000 ./Core/Src/main.o\n")
        self.state = os.path.join(r, "state")
        self.data = slurp(DATA)
        self.board(self.data)
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
        spit(os.path.join(self.state, "board.txt"), data)

    def run_sh(self, *args, **env):
        e = dict(os.environ)
        e.update(PATH=os.path.join(self.root, "bin") + os.pathsep + e["PATH"],
                 STM32_PROG=os.path.join(self.root, "bin", "STM32_Programmer_CLI.exe"),
                 FAKE_STATE=self.state, FAKE_ROOT=self.root, FLASH_WAIT_SEC="5")
        e.update(env)
        p = subprocess.run([BASH, os.path.join(self.root, "tools", "read_data.sh")] + list(args),
                           cwd=self.root, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def archive(self):
        d = os.path.join(self.root, "build", "archive")
        return [os.path.join(d, f) for f in sorted(os.listdir(d))] if os.path.isdir(d) else []

    def test_wipe_survives_crlf(self):
        # R1: хеш с диска сверяется не сравнением строк с CR на конце.
        rc, out, err = self.run_sh("--wipe")
        self.assertEqual(rc, 0, out + err)
        self.assertFalse(os.path.exists(os.path.join(self.state, "board.txt")), "архив на плате не стёрт")
        self.assertNotIn("не совпал", err)
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)

    def test_wiped_archive_kept(self):
        # R5: копия, после которой плата стёрта, переживает следующие снятия.
        self.assertEqual(self.run_sh("--wipe")[0], 0)
        arc = self.archive()
        self.assertEqual(len(arc), 1)
        self.assertEqual(slurp(arc[0]), self.data)
        self.assertRegex(os.path.basename(arc[0]), r"^data-\d{8}-\d{6}-[0-9A-F]{8}\.txt$")
        for i in range(2):
            self.board(self.data[:200] + b"next %d\r\n" % i)
            self.assertEqual(self.run_sh()[0], 0)
        self.assertEqual(self.archive(), arc)
        self.assertEqual(slurp(arc[0]), self.data)

    def test_foreign_edit_not_flashed(self):
        # R4: правка main.c во время снятия (здесь — даже не собирающаяся)
        # не уходит на плату; рабочая прошивка — main.c до снятия байт-в-байт,
        # правка возвращается в main.c после прошивки и лежит копией рядом.
        edit = "\n/* BUILD_FAIL: недописано */\n"
        rc, out, err = self.run_sh(FAKE_EDIT=edit)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(slurp(os.path.join(self.state, "fw.c")), self.orig)
        self.assertIn("ИЗМЕНИЛИ", err)
        self.assertEqual(slurp(os.path.join(self.root, "Core", "Src", "main.c")),
                         self.orig + edit.encode("utf-8"))
        yours = [f for f in os.listdir(os.path.join(self.root, "build")) if f.startswith("main.c.yours-")]
        self.assertEqual(len(yours), 1)
        self.assertEqual(slurp(os.path.join(self.root, "build", yours[0])), self.orig + edit.encode("utf-8"))

    def test_battery_fail_not_empty_bank(self):
        # R6: отказ замера батареи — не «0 мВ (255%)».
        rc, out, err = self.run_sh(FAKE_BAT_FAIL="1")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("батарея — замер не удался", out)
        rc, out, err = self.run_sh()
        self.assertIn("батарея 3800 мВ (65%), VDD 3300 мВ", out)


if __name__ == "__main__":
    unittest.main()
