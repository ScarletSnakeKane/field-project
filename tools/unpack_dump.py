# -*- coding: utf-8 -*-
"""Сборка и проверка снятого архива для read_data.sh.

    python tools/unpack_dump.py --raw <кусок.bin> <образ.img> <смещение> <хеш>   (оба — hex)
    python tools/unpack_dump.py --image <образ.img> <data.txt> <data_old.txt> <размер> [хеш]
    python tools/unpack_dump.py --usb <data.txt> <info.txt>
    python tools/unpack_dump.py --fnv <файл>
    python tools/unpack_dump.py --check <файл> <хеш>

--raw кладёт кусок сырого образа флеш (dump_info.raw_*, см. dump_patch.py)
в файл образа по его смещению, если хеш сошёлся; образ создаётся и
дорастает по мере кусков. Испорченный кусок не пишется вовсе — испорченный
образ хуже отсутствующего.

--image проверяет, что образ полный (ровно <размер> байт — сколько флеш
заявил дампер) и, если дан хеш, совпадает с флеш целиком (dump_info.flash_hash),
разбирает журнал (tools/log_image.py) и пишет те же data.txt и data_old.txt,
что отдал бы том USB (data_old.txt — только если в журнале есть текст старого
архива). Код возврата 0 — образ полный, журнал разобран; 1 — нет. По коду 0
и только по нему read_data.sh --wipe идёт дальше.

--usb проверяет data.txt, скопированный с тома USB: заголовок тот, что пишет
прошивка, число строк и последняя метка — как в info.txt того же тома.
Сорванное копирование (обрыв кабеля, телефон отдал полфайла) иначе
выглядело бы как свежий, только короткий архив.

--fnv печатает FNV-1a 32 файла — тот же, что считает дампер. --check
сверяет его сам и отвечает только кодом возврата (0 — совпал). Сравнивать в
bash вывод --fnv нельзя: на Windows print() пишет CRLF, Git Bash в $(...)
срезает только LF, и строка с CR на конце никогда не равна хешу (R1).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_image  # noqa: E402


def fnv1a(data):
    """FNV-1a 32 — тот же, что считает дампер (tools/dump_patch.py)."""
    h = 2166136261
    for b in bytearray(data):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def raw_chunk(chunk_path, img, off_hex, hash_hex):
    chunk = slurp(chunk_path)
    # Шестнадцатеричные, как их печатает -r32 (без 0x; с 0x тоже можно):
    # int(x, 0) не разобрал бы «00008000», а «12345678» прочёл бы десятичным.
    off, want = int(off_hex, 16), int(hash_hex, 16)
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


def image(img_path, out, out_old, size, hash_hex=None):
    if not os.path.exists(img_path):
        print("   !! образа нет: %s" % img_path, file=sys.stderr)
        return 1
    img = slurp(img_path)
    if len(img) != size:
        print("   !! образ неполный: %d Б из %d — не все куски сняты" % (len(img), size), file=sys.stderr)
        return 1
    if hash_hex is not None and fnv1a(img) != int(hash_hex, 16):
        print("   !! образ на диске (0x%08X) не совпал с флеш целиком (0x%s)"
              % (fnv1a(img), hash_hex), file=sys.stderr)
        return 1
    try:
        j = log_image.Journal(img)
    except ValueError as e:
        print("   !! %s" % e, file=sys.stderr)
        return 1
    vol = log_image.Volume(j)
    for line in log_image.summary(j, vol):
        print("   " + line)
    if j.state == "foreign":
        return 1
    with open(out, "wb") as f:
        f.write(vol.data)
    if os.path.exists(out_old):
        os.remove(out_old)   # прошлый снимок не должен выдавать себя за этот
    if vol.old:
        with open(out_old, "wb") as f:
            f.write(vol.old)
        print("   текст старого архива: %s" % out_old)
    if vol.import_pending():
        # Старый том ещё цел только на флеш: в журнале его текст не весь.
        print("   !! импорт старого архива не закончен: прошивка повторит его при питании;"
              " старый том целиком — в образе (mcopy -i <образ>@@32256)", file=sys.stderr)
        return 1
    return 0


def usb(data_path, info_path):
    data = slurp(data_path)
    info = slurp(info_path).decode("ascii", "replace")
    if "ARCHIVE NOT AVAILABLE" in info:
        print("   !! том сообщает: %s" % info.strip().splitlines()[1], file=sys.stderr)
        return 1
    m = re.search(r"data\.txt: (\d+) lines, first (\S+ \S+|-), last (\S+ \S+|-)", info)
    if not m:
        print("   !! info.txt не разобран — сверить data.txt не с чем", file=sys.stderr)
        return 1
    want_n, want_last = int(m.group(1)), m.group(3)
    head = (log_image.CSV_HEADER + "\r\n").encode("ascii")
    if not data.startswith(head):
        print("   !! заголовок data.txt не тот, что пишет прошивка", file=sys.stderr)
        return 1
    if not data.endswith(b"\r\n"):
        print("   !! data.txt обрывается посреди строки — копирование не закончено", file=sys.stderr)
        return 1
    lines = data[len(head):].split(b"\r\n")[:-1]
    if len(lines) != want_n:
        print("   !! в data.txt %d строк, а том заявляет %d — копия неполная" % (len(lines), want_n),
              file=sys.stderr)
        return 1
    last = lines[-1][:19].decode("ascii", "replace") if lines else "-"
    if last != want_last:
        print("   !! последняя строка %s, а том заявляет %s" % (last, want_last), file=sys.stderr)
        return 1
    print("   сверено с info.txt: %d строк, последняя %s" % (want_n, want_last))
    return 0


def main(a):
    if not a:
        print(__doc__)
        return 2
    if a[0] == "--fnv":
        print("0x%08X" % fnv1a(slurp(a[1])))
        return 0
    if a[0] == "--check":
        return 0 if fnv1a(slurp(a[1])) == int(a[2], 0) else 1
    if a[0] == "--raw" and len(a) == 5:
        raw_chunk(*a[1:])
        return 0
    if a[0] == "--image" and len(a) in (5, 6):
        return image(a[1], a[2], a[3], int(a[4], 0), a[5] if len(a) == 6 else None)
    if a[0] == "--usb" and len(a) == 3:
        return usb(a[1], a[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
