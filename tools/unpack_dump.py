# -*- coding: utf-8 -*-
"""Собрать снятый дамп в читаемый CSV.

Когда архив влезает в буфер целиком, работы здесь нет — байты просто
переписываются. Когда не влезает, дампер отдаёт не хвост, а окна, равномерно
разложенные по всей длине файла (см. tools/dump_patch.py). Окна режутся по
границе байта, а не строки, поэтому на стыках всегда оказывается полстроки:
их надо отбросить, иначе разбор получит склеенную из двух половинок строку,
которая по числу полей выглядит правдоподобно и потому особенно вредна.

    python tools/unpack_dump.py <вход.bin> <выход.csv> <win_bytes> <win_count> [хеш]
    python tools/unpack_dump.py --fnv <файл>
    python tools/unpack_dump.py --check <файл> <хеш>
    python tools/unpack_dump.py --raw <кусок.bin> <образ.img> <смещение> <хеш>

win_bytes = 0 означает непрерывный дамп. Хеш — dump_info.hash из дампера:
если прочитанные по SWD байты с ним не сходятся (не тот адрес, обрыв чтения),
выходной файл не пишется вовсе — испорченный архив хуже отсутствующего.
--fnv печатает тот же хеш для файла на диске. --check сверяет хеш файла сам
и отвечает только кодом возврата (0 — совпал). Сравнивать в bash вывод --fnv
нельзя: на Windows print() пишет CRLF, Git Bash в $(...) срезает только LF,
и строка с CR на конце никогда не равна хешу (R1).
--raw кладёт кусок сырого образа флеш (dump_info.raw_*, см. dump_patch.py)
в файл образа по его смещению, если хеш сошёлся; образ создаётся и
дорастает по мере кусков. Нужен, когда том не монтируется (fs_ok = 0):
архив тогда вытаскивается из образа на компьютере, а не на плате.
"""
import os
import sys


def fnv1a(data):
    """FNV-1a 32 — тот же, что считает дампер (tools/dump_patch.py)."""
    h = 2166136261
    for b in bytearray(data):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def main():
    if sys.argv[1] == "--fnv":
        print("0x%08X" % fnv1a(open(sys.argv[2], "rb").read()))
        return
    if sys.argv[1] == "--check":
        got = fnv1a(open(sys.argv[2], "rb").read())
        sys.exit(0 if got == int(sys.argv[3], 0) else 1)
    if sys.argv[1] == "--raw":
        chunk = open(sys.argv[2], "rb").read()
        img, off, want = sys.argv[3], int(sys.argv[4], 0), int(sys.argv[5], 0)
        got = fnv1a(chunk)
        if got != want:
            sys.exit("   !! хеш куска (0x%08X) не совпал с хешем на плате (0x%08X):"
                     " кусок не записан" % (got, want))
        # r+b, а не w: остальные куски образа уже на месте.
        mode = "r+b" if os.path.exists(img) else "wb"
        with open(img, mode) as f:
            f.seek(off)
            f.write(chunk)
        print("   кусок 0x%06X..0x%06X записан в %s" % (off, off + len(chunk), img))
        return
    src, dst = sys.argv[1], sys.argv[2]
    win_bytes = int(sys.argv[3])
    win_count = int(sys.argv[4])

    data = open(src, "rb").read()
    if len(sys.argv) > 5:
        want = int(sys.argv[5], 0)
        got = fnv1a(data)
        if got != want:
            sys.exit("   !! хеш снятого (0x%08X) не совпал с хешем на плате (0x%08X):"
                     " ОЗУ прочитано не то или не целиком, файл не записан" % (got, want))

    if win_bytes == 0 or win_count <= 1:
        open(dst, "wb").write(data)
        print("   снято подряд, без пропусков")
        return

    header = None
    rows = []
    dropped = 0

    for i in range(win_count):
        chunk = data[i * win_bytes:(i + 1) * win_bytes]
        if not chunk:
            break
        lines = chunk.split(b"\n")

        # Первое окно начинается с нуля файла, значит его первая строка целая
        # и это заголовок. У всех остальных окон первая строка обрезана слева.
        if i == 0:
            if lines and lines[0].strip():
                header = lines[0].rstrip(b"\r")
            body = lines[1:]
        else:
            body = lines[1:]
            dropped += 1

        # Последняя строка любого окна обрезана справа — отбрасываем всегда.
        if body:
            dropped += 1
            body = body[:-1]

        for l in body:
            l = l.rstrip(b"\r")
            if l.strip():
                rows.append(l)

    if header is None:
        print("   !! заголовок не найден в первом окне", file=sys.stderr)
        header = b""

    out = b"\r\n".join([header] + rows) + b"\r\n"
    open(dst, "wb").write(out)
    print("   собрано %d строк из %d окон (обрезков отброшено: %d)"
          % (len(rows), win_count, dropped))


main()
