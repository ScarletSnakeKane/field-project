# -*- coding: utf-8 -*-
"""Собрать снятый дамп в читаемый CSV.

Когда архив влезает в буфер целиком, работы здесь нет — байты просто
переписываются. Когда не влезает, дампер отдаёт не хвост, а окна, равномерно
разложенные по всей длине файла (см. tools/dump_patch.py). Окна режутся по
границе байта, а не строки, поэтому на стыках всегда оказывается полстроки:
их надо отбросить, иначе разбор получит склеенную из двух половинок строку,
которая по числу полей выглядит правдоподобно и потому особенно вредна.

    python tools/unpack_dump.py <вход.bin> <выход.csv> <win_bytes> <win_count>

win_bytes = 0 означает непрерывный дамп.
"""
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    win_bytes = int(sys.argv[3])
    win_count = int(sys.argv[4])

    data = open(src, "rb").read()

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
