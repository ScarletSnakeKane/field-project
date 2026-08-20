# -*- coding: utf-8 -*-
"""Сводка по снятому архиву.

Смысл не в красоте вывода, а в том, чтобы не приходилось читать сам файл
целиком: в нём тысячи одинаковых строк, и глазами (равно как и языковой
моделью) в них всё равно ищут одно и то же — есть ли ошибки, ровный ли
интервал, в каких пределах гуляют показания.

    python tools/summarize_data.py [файл]      (по умолчанию build/data.txt)
"""
import collections
import datetime as dt
import sys

# Колонки, где любое ненулевое значение — это отказ, а не показание.
ERROR_COLS = ("aht_init_st", "aht_read_st", "ds_err", "write_fails",
              "aht_i2c_err")
# Колонки-флаги, где отказом является НОЛЬ.
FLAG_COLS = ("soil_valid", "lse_ok")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/data.txt"
    raw = open(path, "rb").read()

    text = raw.decode("ascii", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = [l for l in text.split("\n") if l.strip()]
    if len(lines) < 2:
        print("файл пуст или содержит только заголовок")
        return 1

    header = lines[0].split(",")
    rows = [l.split(",") for l in lines[1:]]
    col = {n: [r[i] if i < len(r) else "" for r in rows]
           for i, n in enumerate(header)}

    ragged = sum(1 for r in rows if len(r) != len(header))
    avg = sum(len(l) + 2 for l in lines[1:]) / len(rows)
    print("%s: %d Б, %d строк, %d колонок, средняя строка %.0f Б"
          % (path, len(raw), len(rows), len(header), avg))
    if ragged:
        print("  !! строк с неверным числом полей: %d" % ragged)

    # Двойной перевод строки — след давней ошибки: f_puts сам разворачивает \n
    # в \r\n, и лишний \r в формате давал "\r\r\n".
    if raw.count(b"\r") != raw.count(b"\n"):
        print("  !! CR (%d) != LF (%d) — двойной перевод строки"
              % (raw.count(b"\r"), raw.count(b"\n")))

    bad = []
    for name in ERROR_COLS:
        if name in col:
            n = sum(1 for v in col[name] if v.strip() not in ("0", "0x00", ""))
            if n:
                bad.append("%s != 0 в %d строках" % (name, n))
    for name in FLAG_COLS:
        if name in col:
            n = sum(1 for v in col[name] if v.strip() in ("0", ""))
            if n:
                bad.append("%s == 0 в %d строках" % (name, n))
    print("  отказы: " + ("; ".join(bad) if bad else "нет"))

    for name in header:
        vals = []
        for v in col[name]:
            try:
                vals.append(float(v))
            except ValueError:
                pass
        if len(vals) == len(rows) and name != "timestamp":
            lo, hi = min(vals), max(vals)
            if lo != hi:
                print("  %-14s %8.2f .. %8.2f" % (name, lo, hi))

    if "timestamp" in col:
        try:
            ts = [dt.datetime.strptime(t, "%Y-%m-%d %H:%M:%S")
                  for t in col["timestamp"]]
        except ValueError:
            print("  метки времени не разбираются")
            return 0
        gaps = [(ts[i + 1] - ts[i]).total_seconds() for i in range(len(ts) - 1)]
        if gaps:
            common = collections.Counter(gaps).most_common(3)
            print("  интервал, с: " +
                  ", ".join("%g (x%d)" % (g, n) for g, n in common))
            # Время назад — это перепрошивка: часы переставляются на время
            # сборки. Не поломка, но метки через обновление не монотонны.
            back = sum(1 for g in gaps if g < 0)
            if back:
                print("  время шло назад %d раз (след перепрошивки)" % back)
        print("  период: %s .. %s" % (ts[0], ts[-1]))
    return 0


sys.exit(main())
