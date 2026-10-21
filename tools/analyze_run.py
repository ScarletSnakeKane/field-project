# -*- coding: utf-8 -*-
"""Разбор снятого архива: отказы, интервалы, разряд батареи, срок работы.

Этот разбор приходится делать после каждого прогона, и каждый раз одинаково,
поэтому он живёт здесь, а не пишется заново по месту.

    python tools/analyze_run.py <файл> [--from "ГГГГ-ММ-ДД ЧЧ:ММ"] [--to ...]
    python tools/analyze_run.py --compare <стенд.csv> <боевой.csv>

Границы окна нужны чаще, чем кажется: в архиве обычно перемешаны стендовый и
боевой участки, а хвост испорчен возней с программатором — во время прошивки
плата не спит и просаживает банку, и эти точки нельзя пускать в регрессию.

ПРО ЗАМЕР ТОКА: считается он по наклону разряда, поэтому меряет всё, что висит
на банке. Подключённый ST-Link тянет порядка 1.5 мА и полностью забивает
собственное потребление прибора. Любой прогон, по которому делаются выводы о
сроке работы, должен идти с ФИЗИЧЕСКИ ОТКЛЮЧЁННЫМ отладчиком.
"""
import collections
import datetime as dt
import math
import os
import re
import statistics as st
import sys

# Заряд считается той же кривой, что и в прошивке: таблица читается прямо из
# Core/Src/battery_curve.c, чтобы две копии не разошлись. Раньше здесь стояла
# константа 8 %/100 мВ на весь диапазон, а по кривой на рабочем участке
# 3.9-3.5 В выходит 12-14 %/100 мВ; вдобавок срок считался от 100 %, а не от
# текущего заряда. Вместе это завышало срок в 1.5-3 раза (D2).
CURVE_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "Core", "Src", "battery_curve.c")


def load_curve(path=CURVE_SRC):
    src = open(path, encoding="utf-8").read()
    m = re.search(r"bat_curve\[\]\s*=\s*\{(.*?)\};", src, re.S)
    pts = re.findall(r"\{\s*([0-9.]+)f?\s*,\s*(\d+)\s*\}", m.group(1)) if m else []
    if len(pts) < 2:
        sys.exit("не разобрал таблицу bat_curve в %s" % path)
    return [(float(v), float(p)) for v, p in pts]


CURVE = load_curve()


def volts_to_pct(v):
    """Как Battery_VoltsToPercent, но без округления до целого: целые проценты
    на коротком прогоне дали бы ступеньки вместо наклона."""
    if v >= CURVE[0][0]:
        return CURVE[0][1]
    if v <= CURVE[-1][0]:
        return CURVE[-1][1]
    for (v1, p1), (v0, p0) in zip(CURVE, CURVE[1:]):
        if v >= v0:
            return p0 + (p1 - p0) * (v - v0) / (v1 - v0)
    return CURVE[-1][1]

ERROR_COLS = ("aht_init_st", "aht_read_st", "ds_err", "write_fails", "aht_i2c_err")
FLAG_COLS = ("soil_valid", "lse_ok")


def load(path, lo=None, hi=None):
    raw = open(path, "rb").read()
    text = raw.decode("ascii", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = [l for l in text.split("\n") if l.strip()]
    head = lines[0].split(",")
    rows = [l.split(",") for l in lines[1:] if len(l.split(",")) == len(head)]
    col = {n: [r[i] for r in rows] for i, n in enumerate(head)}
    ts = [dt.datetime.strptime(x, "%Y-%m-%d %H:%M:%S") for x in col["timestamp"]]
    keep = [i for i in range(len(ts))
            if (lo is None or ts[i] >= lo) and (hi is None or ts[i] <= hi)]
    return head, {n: [col[n][i] for i in keep] for n in head}, [ts[i] for i in keep], raw


# Значение DS18B20 по сбросу: преобразование не выполнилось, а не 85 °C почвы.
DS_RESET = 85.0


def num(col, name):
    """Числа колонки; None — не число. nan тоже None: float("nan") разбирается
    без ошибки, а потом min/max молча зависят от того, в какой строке он стоит."""
    out = []
    for v in col.get(name, []):
        try:
            f = float(v)
        except ValueError:
            f = None
        out.append(None if f is None or math.isnan(f) or math.isinf(f) else f)
    return out


def fit(hours, ys):
    """МНК-прямая. Возвращает (k, b0, se, sd, n) в единицах ys за час."""
    pairs = [(h, v) for h, v in zip(hours, ys) if v is not None]
    if len(pairs) < 3:
        return None
    x = [p[0] for p in pairs]
    y = [p[1] for p in pairs]
    mx, my = st.mean(x), st.mean(y)
    sxx = sum((a - mx) ** 2 for a in x)
    if sxx == 0:
        return None
    k = sum((a - mx) * (b - my) for a, b in zip(x, y)) / sxx
    b0 = my - k * mx
    resid = [b - (k * a + b0) for a, b in zip(x, y)]
    sd = st.pstdev(resid)
    se = (sd / sxx ** 0.5) * (len(x) / max(1, len(x) - 2)) ** 0.5
    return k, b0, se, sd, len(x)


def regress(hours, volts):
    """Наклон в мВ/ч со стандартной ошибкой. Возвращает (k, se, sd, n)."""
    r = fit(hours, volts)
    if not r:
        return None
    k, _, se, sd, n = r
    return k * 1000.0, se * 1000.0, sd * 1000.0, n


def charge(hours, volts):
    """Разряд в процентах по кривой прошивки: (%/ч, заряд в начале, в конце).
    Расход в %/ч постоянен при постоянном токе, а мВ/ч — нет: кривая на разных
    участках разная, поэтому срок и сравнение режимов считаются в процентах."""
    pct = [None if v is None else volts_to_pct(v) for v in volts]
    r = fit(hours, pct)
    if not r:
        return None
    k, b0 = r[0], r[1]
    clamp = lambda p: min(100.0, max(0.0, p))
    return k, clamp(b0 + k * hours[0]), clamp(b0 + k * hours[-1])


def days_left(pct_per_hour, pct_now):
    """Сколько суток до нуля от текущего заряда при том же расходе."""
    if pct_per_hour >= 0:
        return None
    return pct_now / -pct_per_hour / 24.0


def ranges(head, col, nrows, skip=()):
    """Диапазоны колонок без nan и без заведомого мусора; nan считаются отдельно."""
    for n in head:
        if n == "timestamp" or n in skip:
            continue
        raw = col[n]
        vals = num(col, n)
        nans = sum(1 for v in raw if v.strip().lower() in ("nan", "-nan", "inf", "-inf"))
        good = [v for v in vals if v is not None]
        # Колонка числовая, если всё в ней — число или nan.
        if len(good) + nans != nrows:
            continue
        junk = 0
        if n == "soil_temp":
            junk = sum(1 for v in good if v == DS_RESET)
            good = [v for v in good if v != DS_RESET]
        note = ""
        if nans:
            note += "   nan: %d" % nans
        if junk:
            note += "   85.00 (сброс DS18B20): %d" % junk
        if not good:
            print("  %-14s %s%s" % (n, "нет ни одного значения", note))
        elif min(good) != max(good) or note:
            print("  %-14s %8.2f .. %8.2f%s" % (n, min(good), max(good), note))


def report(path, lo, hi):
    head, col, ts, raw = load(path, lo, hi)
    if not ts:
        print("в заданном окне нет строк")
        return
    print("%s: %d строк, %s .. %s" % (path, len(ts), ts[0], ts[-1]))

    if raw.count(b"\r") != raw.count(b"\n"):
        print("  !! CR (%d) != LF (%d) — двойной перевод строки"
              % (raw.count(b"\r"), raw.count(b"\n")))

    bad = []
    for n in ERROR_COLS:
        if n in col:
            k = sum(1 for v in col[n] if v.strip() not in ("0", "0x00", ""))
            if k:
                bad.append("%s != 0 в %d из %d" % (n, k, len(ts)))
    for n in FLAG_COLS:
        if n in col:
            k = sum(1 for v in col[n] if v.strip() in ("0", ""))
            if k:
                bad.append("%s == 0 в %d из %d" % (n, k, len(ts)))
    print("  отказы: " + ("; ".join(bad) if bad else "нет"))

    # Значения, которые физически невозможны и означают мусор, а не измерение.
    for n, lim, why in (("air_temp", -40.0, "пустой кадр AHT20 даёт -50.00"),
                        ("soil_temp", -40.0, "мёртвая шина 1-Wire")):
        if n in col:
            k = sum(1 for v in num(col, n) if v is not None and v < lim)
            if k:
                print("  !! %s < %g в %d строках (%s)" % (n, lim, k, why))
    if "soil_temp" in col:
        k = sum(1 for v in num(col, "soil_temp") if v == DS_RESET)
        if k:
            print("  !! soil_temp = 85.00 в %d строках (значение DS18B20 по сбросу:"
                  " преобразование не выполнилось) — в диапазон не входит" % k)

    ranges(head, col, len(ts))

    gaps = [(ts[i + 1] - ts[i]).total_seconds() for i in range(len(ts) - 1)]
    if gaps:
        common = collections.Counter(gaps).most_common(3)
        print("  интервал, с: " + ", ".join("%g (x%d)" % g for g in common))
        back = sum(1 for g in gaps if g < 0)
        if back:
            print("  время шло назад %d раз (след перепрошивки: часы встают на время сборки)" % back)

    if "vbat" in col:
        hours = [(t - ts[0]).total_seconds() / 3600.0 for t in ts]
        r = regress(hours, num(col, "vbat"))
        if r:
            k, se, sd, n = r
            print()
            print("  РАЗРЯД: %+.3f мВ/ч (ст. ошибка %.3f), разброс точек %.1f мВ, %d точек за %.1f ч"
                  % (k, se, sd, n, hours[-1]))
            c = charge(hours, num(col, "vbat"))
            if c:
                kp, p0, p1 = c
                print("  по кривой прошивки: %.0f %% -> %.0f %%, %+.4f %%/ч" % (p0, p1, kp))
                d = days_left(kp, p1)
                if d is not None:
                    print("  осталось от текущих %.0f %%: ~%.0f суток" % (p1, d))
                    print("  (полная банка при том же расходе: ~%.0f суток)" % days_left(kp, 100.0))
                    print("  (наклон меряет ВСЁ, что висит на банке — при подключённом ST-Link это в основном он)")
                elif kp == 0 and p0 >= 100.0:
                    print("  банка выше %.2f В — кривая там плоская, срок по наклону не оценить" % CURVE[0][0])


def compare(a, b):
    """Сравнение двух режимов на одной банке: систематика сокращается."""
    out = []
    for p in (a, b):
        head, col, ts, _ = load(p, None, None)
        hours = [(t - ts[0]).total_seconds() / 3600.0 for t in ts]
        r = regress(hours, num(col, "vbat"))
        c = charge(hours, num(col, "vbat"))
        gaps = [(ts[i + 1] - ts[i]).total_seconds() for i in range(len(ts) - 1)]
        hist = collections.Counter(gaps).most_common(2) if gaps else [(0, 0)]
        period = hist[0][0]
        out.append((p, c, period))
        if r and c:
            print("%s: цикл %g с, наклон %+.3f мВ/ч = %+.4f %%/ч (заряд %.0f..%.0f %%) по %d точкам"
                  % (p, period, r[0], c[0], c[1], c[2], r[3]))
        else:
            print("%s: цикл %g с, наклон не определяется" % (p, period))

        # Сравнивать имеет смысл только однорежимные куски. В архиве обычно
        # лежат вперемешку стендовый и боевой участки, и регрессия по такому
        # файлу описывает несуществующий средний режим. Вырезать нужное окно
        # заранее: analyze_run.py <файл> --from ... --to ... покажет границы.
        if len(hist) > 1 and hist[1][1] > 0.1 * hist[0][1]:
            print("   !! в файле два разных интервала (%g с x%d и %g с x%d) —"
                  " это смесь режимов, сравнение будет бессмысленным"
                  % (hist[0][0], hist[0][1], hist[1][0], hist[1][1]))
    # Отношение токов — это отношение наклонов в %/ч. Отношение в мВ/ч совпадает
    # с ним только на одном и том же участке кривой: стенд на 4.1 В и поле на
    # 3.8 В давали ошибку примерно в 1.5 раза (D2).
    (_, ca, pa), (_, cb, pb) = out
    if ca and cb and cb[0] != 0:
        ratio = abs(ca[0] / cb[0])
        print()
        print("расход в %%/ч отличается в %.1f раза" % ratio)
        print("Если известен средний ток первого режима, ток второго = первый / %.1f." % ratio)
        print("Расхождение с амперметром обычно означает, что при одном из замеров")
        print("был подключён ST-Link: он тянет из платы около 1.5 мА.")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    if args[0] == "--compare":
        if len(args) != 3:
            print("нужно два файла")
            return 2
        compare(args[1], args[2])
        return 0
    path = args[0]
    lo = hi = None
    for i, a in enumerate(args):
        if a == "--from" and i + 1 < len(args):
            lo = dt.datetime.strptime(args[i + 1], "%Y-%m-%d %H:%M")
        if a == "--to" and i + 1 < len(args):
            hi = dt.datetime.strptime(args[i + 1], "%Y-%m-%d %H:%M")
    report(path, lo, hi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
