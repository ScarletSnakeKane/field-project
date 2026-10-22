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
import math
import sys

# Колонки, где любое ненулевое значение — это отказ, а не показание.
# Архивы прежних прошивок (data_N.txt) части колонок не имеют — это не ошибка.
ERROR_COLS = ("aht_init_st", "aht_read_st", "ds_err", "air_xchk", "i2c_rec",
              "sens_off", "write_fails", "lowpower_skips", "aht_i2c_err")
# Колонки-флаги, где отказом является НОЛЬ.
FLAG_COLS = ("soil_valid", "lse_ok", "time_ok")
# Коды и счётчики, у которых «диапазон» ничего не говорит: их разбирают
# отдельно (отказы, старты), а в диапазонах они только шумят.
NOT_RANGES = ("aht_init_st", "aht_read_st", "ds_err", "air_xchk", "i2c_rec",
              "sens_off", "reset_cause", "boot_count", "soil_valid", "lse_ok",
              "time_ok")
# Значение DS18B20 по сбросу: преобразование не выполнилось, а не 85 °C почвы.
DS_RESET = 85.0

# Коды ds_err — DS18B20_Status из Core/Inc/ds18b20_decode.h. Менять вместе.
# Без расшифровки сводка говорила только «ds_err != 0», и, например, клон
# датчика (код 5 каждый час) было не отличить от оборванного провода (R9).
DS_ERR_TEXT = {
    1: "нет presence перед измерением",
    2: "нет presence перед чтением",
    3: "CRC не сошёлся",
    4: "шина читается нулями — нет подтяжки или датчика",
    5: "служебные байты scratchpad не те — кадр битый или не DS18B20",
    6: "85.00 °C по сбросу — преобразование не прошло",
    7: "вне шкалы -55..+125 °C",
    8: "не опрашивался — карантин питания датчиков (sens_off > 0)",
}

# Коды прочих диагностических колонок — из Core/Src/main.c (описание
# CSV_HEADER) и Core/Inc/sensor_policy.h. Менять вместе.
CODE_TEXT = {
    "ds_err": DS_ERR_TEXT,
    "air_xchk": {
        1: "AHT20 и BMP280 разошлись больше 2 °C — AHT20 под подозрением",
        2: "BMP280 не ответил — сверки нет",
        3: "у AHT20 нет значения — сверять нечего",
    },
    "i2c_rec": {
        1: "шина I2C освобождена тактами SCL",
        2: "шину освободило только снятие питания датчиков",
        3: "шина I2C не освободилась",
    },
}

# reset_cause: биты 0..6 — флаги сброса, 8..11 — код сбоя (Policy_ResetCause
# в Core/Src/power_policy.c).
RESET_BITS = ((1, "питание"), (2, "NRST"), (4, "просадка"), (8, "программный"),
              (16, "сторож"), (32, "WWDG"), (64, "LPWR"))
FAULT_TEXT = {1: "HardFault", 2: "MemManage", 3: "BusFault", 4: "UsageFault",
              5: "NMI", 6: "Error_Handler"}


def reset_text(code):
    """«питание» или «программный, HardFault». Подача питания на F401 взводит
    и NRST, и просадку — это одно событие, не три. Программный сброс, сторож
    и WWDG/LPWR сами дёргают вывод NRST, так что PINRSTF при них — не нажатие
    кнопки RESET, и в тексте не показывается."""
    if code & 1:
        flags = "питание"
    else:
        if code & (8 | 16 | 32 | 64):
            code &= ~2
        flags = "+".join(t for b, t in RESET_BITS if code & b) or "без флагов"
    fault = (code >> 8) & 0xF
    return flags + (", " + FAULT_TEXT.get(fault, "сбой %d" % fault) if fault else "")


def starts(col):
    """Старты прибора, видимые в архиве. Возвращает Counter причин или None,
    если колонок нет.

    Старт — строка, где boot_count сменился. Но подача питания обнуляет
    backup-домен, и два старта от питания подряд (включили, потом заменили
    банку) дают одинаковый boot_count = 1. Их различаем по следам потери
    календаря: метка времени пошла назад или time_ok сменился с 1 на 0.
    Потеря питания без таких следов (банку заменили до первой строки после
    прошивки) не видна — это предел того, что есть в архиве."""
    if "boot_count" not in col or "reset_cause" not in col:
        return None
    ts = col.get("timestamp", [""] * len(col["boot_count"]))
    tok = col.get("time_ok", ["1"] * len(col["boot_count"]))
    cnt = collections.Counter()
    prev = None
    for i, (b, r) in enumerate(zip(col["boot_count"], col["reset_cause"])):
        new = (i == 0) or b != prev
        if not new and b.strip() == "1":
            new = ts[i] < ts[i - 1] or (tok[i - 1].strip() == "1" and tok[i].strip() == "0")
        if new:
            try:
                cnt[reset_text(int(r))] += 1
            except ValueError:
                cnt["не число"] += 1
        prev = b
    return cnt


def time_untrusted(col):
    """Номера строк, меткам которых верить нельзя (time_ok = 0, A6)."""
    return {i for i, v in enumerate(col.get("time_ok", [])) if v.strip() == "0"}


def code_detail(values, table):
    """Разбивка ненулевых кодов: «код 5 (…) x24; код 1 (…) x2»."""
    cnt = collections.Counter(v.strip() for v in values
                              if v.strip() not in ("0", "0x00", ""))
    parts = []
    for v, n in cnt.most_common():
        try:
            text = table.get(int(v, 0), "неизвестный код")
        except ValueError:
            text = "не число"
        parts.append("код %s (%s) x%d" % (v, text, n))
    return "; ".join(parts)


def ds_err_detail(values):
    return code_detail(values, DS_ERR_TEXT)


def failures(col, total=None):
    """Строки «отказы: …» — общие для сводки и analyze_run.py. total — если
    задан, к числу строк дописывается « из total»."""
    of = (" из %d" % total) if total is not None else ""
    bad = []
    for name in ERROR_COLS:
        if name in col:
            n = sum(1 for v in col[name] if v.strip() not in ("0", "0x00", ""))
            if n:
                line = "%s != 0 в %d%s" % (name, n, of)
                if total is None:
                    line += " строках"
                if name in CODE_TEXT:
                    line += " (%s)" % code_detail(col[name], CODE_TEXT[name])
                bad.append(line)
    for name in FLAG_COLS:
        if name in col:
            n = sum(1 for v in col[name] if v.strip() in ("0", ""))
            if n:
                bad.append("%s == 0 в %d%s" % (name, n, of if total is not None else " строках"))
    return bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    # Когда архив не влез в буфер, дампер отдаёт окна по всей длине файла,
    # а не сплошной кусок. Разрывы между окнами тогда — след способа снятия,
    # а не пропущенные записи, и подавать их как пропуски значит пугать зря.
    windowed = "--windowed" in sys.argv

    path = args[0] if args else "build/data.txt"
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

    bad = failures(col)
    print("  отказы: " + ("; ".join(bad) if bad else "нет"))

    st = starts(col)
    if st:
        print("  стартов в архиве: %d (%s)" % (sum(st.values()),
              ", ".join("%s x%d" % (t, n) for t, n in st.most_common())))

    # nan отказавшего датчика разбирается float() без ошибки, но в min/max
    # ведёт себя по-разному в зависимости от позиции: в первой строке он давал
    # «air_temp nan .. nan» при 199 валидных строках (D8). Поэтому nan и 85.00
    # почвы в диапазон не входят, а считаются отдельно.
    for name in header:
        if name == "timestamp" or name in NOT_RANGES:
            continue
        vals = []
        nans = 0
        for v in col[name]:
            try:
                f = float(v)
            except ValueError:
                continue
            if math.isnan(f) or math.isinf(f):
                nans += 1
            else:
                vals.append(f)
        if len(vals) + nans != len(rows):
            continue
        note = ""
        if nans:
            note += "   nan: %d" % nans
        if name == "soil_temp":
            junk = sum(1 for v in vals if v == DS_RESET)
            vals = [v for v in vals if v != DS_RESET]
            if junk:
                note += "   85.00 (сброс DS18B20): %d" % junk
        if not vals:
            print("  %-14s нет ни одного значения%s" % (name, note))
        elif min(vals) != max(vals) or note:
            print("  %-14s %8.2f .. %8.2f%s" % (name, min(vals), max(vals), note))

    if "timestamp" in col:
        # Строки с time_ok = 0 несут метки откатившихся на время сборки часов
        # (A6): в интервалы и период они не входят, иначе «время назад» и
        # месячные «пропуски» были бы следом не прибора, а замены банки.
        skip = time_untrusted(col)
        if skip:
            print("  меткам %d строк не верить (time_ok = 0) — в интервалы не входят" % len(skip))
        try:
            ts = [dt.datetime.strptime(t, "%Y-%m-%d %H:%M:%S")
                  for i, t in enumerate(col["timestamp"]) if i not in skip]
        except ValueError:
            print("  метки времени не разбираются")
            return 0
        if not ts:
            return 0
        gaps = [(ts[i + 1] - ts[i]).total_seconds() for i in range(len(ts) - 1)]
        if gaps:
            common = collections.Counter(gaps).most_common(3)
            if windowed:
                # Оставляем только рабочий интервал: остальное — стыки окон.
                modal = common[0][0]
                jumps = sum(1 for g in gaps if g > modal * 2)
                print("  интервал, с: %g (x%d); %d разрывов — стыки окон, "
                      "не пропуски" % (common[0][0], common[0][1], jumps))
            else:
                print("  интервал, с: " +
                      ", ".join("%g (x%d)" % (g, n) for g, n in common))
            # Время назад — это перепрошивка: часы переставляются на время
            # сборки. Не поломка, но метки через обновление не монотонны.
            back = sum(1 for g in gaps if g < 0)
            if back:
                print("  время шло назад %d раз (след перепрошивки)" % back)
        print("  период: %s .. %s" % (ts[0], ts[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
