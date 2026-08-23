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
import statistics as st
import sys

# В области 4.0-4.1 В кривая разряда Li-ion даёт примерно столько ёмкости
# на каждые 100 мВ. Грубо, но для сравнения режимов между собой достаточно.
PCT_PER_100MV = 8.0

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


def num(col, name):
    out = []
    for v in col.get(name, []):
        try:
            out.append(float(v))
        except ValueError:
            out.append(None)
    return out


def regress(hours, volts):
    """Наклон в мВ/ч со стандартной ошибкой. Возвращает (k, se, sd, n)."""
    pairs = [(h, v) for h, v in zip(hours, volts) if v is not None]
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
    return k * 1000.0, se * 1000.0, sd * 1000.0, len(x)


def days_left(mv_per_hour):
    """Срок до нуля по наклону. От ёмкости не зависит: она сокращается."""
    if mv_per_hour >= 0:
        return None
    pct_per_hour = abs(mv_per_hour) / 100.0 * PCT_PER_100MV
    return 100.0 / pct_per_hour / 24.0


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

    for n in head:
        vals = [v for v in num(col, n) if v is not None]
        if n != "timestamp" and len(vals) == len(ts) and vals and min(vals) != max(vals):
            print("  %-14s %8.2f .. %8.2f" % (n, min(vals), max(vals)))

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
            d = days_left(k)
            if d:
                print("  срок при таком наклоне: ~%.0f суток" % d)
                print("  (наклон меряет ВСЁ, что висит на банке — при подключённом ST-Link это в основном он)")


def compare(a, b):
    """Сравнение двух режимов на одной банке: систематика сокращается."""
    out = []
    for p in (a, b):
        head, col, ts, _ = load(p, None, None)
        hours = [(t - ts[0]).total_seconds() / 3600.0 for t in ts]
        r = regress(hours, num(col, "vbat"))
        gaps = [(ts[i + 1] - ts[i]).total_seconds() for i in range(len(ts) - 1)]
        hist = collections.Counter(gaps).most_common(2) if gaps else [(0, 0)]
        period = hist[0][0]
        out.append((p, r, period))
        print("%s: цикл %g с, наклон %+.3f мВ/ч по %d точкам" % (p, period, r[0], r[3]))

        # Сравнивать имеет смысл только однорежимные куски. В архиве обычно
        # лежат вперемешку стендовый и боевой участки, и регрессия по такому
        # файлу описывает несуществующий средний режим. Вырезать нужное окно
        # заранее: analyze_run.py <файл> --from ... --to ... покажет границы.
        if len(hist) > 1 and hist[1][1] > 0.1 * hist[0][1]:
            print("   !! в файле два разных интервала (%g с x%d и %g с x%d) —"
                  " это смесь режимов, сравнение будет бессмысленным"
                  % (hist[0][0], hist[0][1], hist[1][0], hist[1][1]))
    (_, ra, pa), (_, rb, pb) = out
    if ra and rb and rb[0] != 0:
        ratio = abs(ra[0] / rb[0])
        print()
        print("наклон отличается в %.1f раза" % ratio)
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


sys.exit(main())
