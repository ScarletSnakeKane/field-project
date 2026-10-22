# -*- coding: utf-8 -*-
"""Разбор сырого образа журнала (П5а) и сборка тех же файлов, что отдаёт том USB (П5б).

Архив на флеш — кольцевой журнал двоичных записей по 64 байта
(Core/Inc/log_record.h, Core/Inc/log_ring.h). По USB прошивка собирает из
него на лету data.txt и data_old.txt (Core/Inc/vfat.h). Здесь то же самое
делается на компьютере из образа флеш, снятого по SWD (read_data.sh --swd):
для диагностики платы, у которой USB не поднимается, и для --wipe.

    python tools/log_image.py <образ.img> [data.txt [data_old.txt]]

Без выходных файлов печатает только сводку журнала.

Раскладка записи — таблицей полей ниже, а не разбором заголовка C: таблицу
сверяет с log_record.h tools/test_tools.py (LogFormat): текстом заголовка
(смещения в LogRecord_PackMeas, константы, LOG_CSV_HEADER) и, где есть gcc,
байт в байт с прошивочным форматтером на случайных записях. Изменится
формат записи в прошивке — тест упадёт, а не этот разбор тихо соврёт.
"""
import math
import struct
import sys
import zlib

RECORD_SIZE = 64
PAYLOAD_OFS = 6
PAYLOAD_SIZE = 54
CRC_OFS = 60
VERSION = 1
SECTOR_SIZE = 4096
SLOTS = SECTOR_SIZE // RECORD_SIZE          # 64
FLASH_SECTORS = 512                         # LOG_FLASH_SECTORS: SST26VF016B, 2 МБ
FLASH_BYTES = FLASH_SECTORS * SECTOR_SIZE

TYPE_MEAS = 0x4D          # 'M'
TYPE_TEXT = 0x54          # 'T'
TYPE_IMPORT_FILE = 0x46   # 'F'
TYPE_IMPORT_END = 0x45    # 'E'
TYPES = (TYPE_MEAS, TYPE_TEXT, TYPE_IMPORT_FILE, TYPE_IMPORT_END)

TEXT_MAX = 52             # LOG_TEXT_MAX
TS_UNKNOWN = "xxxx-xx-xx xx:xx:xx"

CSV_HEADER = ("timestamp,air_temp,air_hum,soil_temp,soil_hum,"
              "bat_pct,vbat,vdd,soil_valid,lse_ok,time_ok,"
              "aht_init_st,aht_read_st,ds_err,air_xchk,i2c_rec,sens_off,"
              "write_fails,lowpower_skips,reset_cause,boot_count,"
              "soil_raw,bat_raw")
COLUMNS = CSV_HEADER.split(",")

# Строка длиннее этого (с \n) у тома не помещается в буфер и выпадает из
# data.txt: VFAT_LINE_BUF - 1 в Core/Inc/vfat.h.
LINE_LIMIT = 447

# Нагрузка замера: (поле LogMeas, смещение от начала нагрузки, вид).
# Вид: "ts" — 6 байт метки, "f32", "u8", "u16", "u32", "raw16" (0xFFFF — nan),
# "bits" — 16 бит флагов и кодов (раскладка — MEAS_BITS).
MEAS_FIELDS = (
    ("ts",             0,  "ts"),
    ("air_temp",       6,  "f32"),
    ("air_hum",        10, "f32"),
    ("soil_temp",      14, "f32"),
    ("soil_hum",       18, "f32"),
    ("bat_volts",      22, "f32"),
    ("vdd",            26, "f32"),
    ("bat_percent",    30, "u8"),
    ("soil_raw",       31, "raw16"),
    ("bat_raw",        33, "raw16"),
    ("bits",           35, "bits"),
    ("sens_off",       37, "u16"),
    ("reset_cause",    39, "u16"),
    ("write_fails",    41, "u32"),
    ("lowpower_skips", 45, "u32"),
    ("boot_count",     49, "u32"),
)
# (поле, младший бит, ширина) внутри "bits".
MEAS_BITS = (
    ("bat_valid", 0, 1), ("bat_low", 1, 1), ("lse_ok", 2, 1), ("time_ok", 3, 1),
    ("aht_init_st", 4, 2), ("aht_read_st", 6, 2), ("ds_err", 8, 4),
    ("air_xchk", 12, 2), ("i2c_rec", 14, 2),
)

REC_ERASED, REC_OK, REC_BAD = 0, 1, 2
READ_OK, READ_BAD = 0, 1
MASK = 0xFFFFFFFF


def crc32(data):
    """CRC32 IEEE — тот же, что LogRecord_Crc32 (как у zip)."""
    return zlib.crc32(bytes(data)) & MASK


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def check(rec):
    """LogRecord_Check: стёрт, годен или испорчен."""
    if rec == b"\xff" * RECORD_SIZE:
        return REC_ERASED
    if u32(rec, CRC_OFS) != crc32(rec[:CRC_OFS]):
        return REC_BAD
    if rec[1] != VERSION or rec[0] not in TYPES:
        return REC_BAD
    return REC_OK


def may_be_ours(rec):
    """LogRecord_MayBeOurs: байт типа — надмножество одного из типов по единицам."""
    return any((rec[0] & t) == t for t in TYPES)


def sdiff(a, b):
    """(int32_t)(a - b) — разность номеров по модулю 2^32."""
    d = (a - b) & MASK
    return d - (1 << 32) if d & 0x80000000 else d


def seq_of(rec):
    return u32(rec, 2)


# ------------------------------------------------------------------ журнал

class Journal:
    """Образ флеш и найденные в нём голова и хвост — как LogRing_Mount."""

    def __init__(self, img):
        if len(img) == 0 or len(img) % SECTOR_SIZE:
            raise ValueError("образ %d Б — не кратен сектору 4 КБ" % len(img))
        self.img = bytes(img)
        self.nsect = len(img) // SECTOR_SIZE
        self.state = "empty"
        self.count = 0
        self.tail_sect = 0
        self.tail_seq = 0
        self.next_seq = 0
        self.head = None
        self._mount()

    def slot(self, sect, k):
        a = sect * SECTOR_SIZE + k * RECORD_SIZE
        return self.img[a:a + RECORD_SIZE]

    def _sector_seq(self, sect):
        """LogRing_SectorSeq: (seq слота 0 или None, есть ли нестёртые слоты)."""
        alien0 = False
        anything = False
        for k in range(SLOTS):
            rec = self.slot(sect, k)
            st = check(rec)
            if st == REC_OK:
                return (seq_of(rec) - k) & MASK, True
            if st == REC_ERASED:
                if k == 0:
                    return None, False
                continue
            anything = True
            if not may_be_ours(rec) and (k == 0 or alien0):
                if k == 0:
                    alien0 = True
                    continue
                if k + 1 < SLOTS:
                    last = self.slot(sect, SLOTS - 1)
                    if check(last) == REC_OK:
                        return (seq_of(last) - (SLOTS - 1)) & MASK, True
                return None, anything
        return None, anything

    def _mount(self):
        n = self.nsect
        head = head_seq0 = None
        foreign = False
        for s in range(n):
            seq0, anything = self._sector_seq(s)
            if seq0 is not None:
                # Наибольший по модулю 2^32, как в прошивке.
                if head is None or sdiff(seq0, head_seq0) > 0:
                    head, head_seq0 = s, seq0
            elif anything:
                foreign = True
        if head is None:
            self.state = "foreign" if foreign else "empty"
            return

        k = SLOTS
        while k > 0 and self.slot(head, k - 1) == b"\xff" * RECORD_SIZE:
            k -= 1
        self.next_seq = (head_seq0 + k) & MASK
        w_sect = (head + 1) % n if k == SLOTS else head
        spare = (w_sect + 1) % n

        t, t_seq0 = head, head_seq0
        i = 1
        while i < n:
            p = (t - 1) % n
            if p == spare or p == w_sect:
                break
            seq0, anything = self._sector_seq(p)
            if seq0 is None:
                pp = (p - 1) % n
                if not anything or i + 1 >= n or pp == spare or pp == w_sect:
                    break
                seq0, anything = self._sector_seq(pp)
                if seq0 is None or seq0 != (t_seq0 - 2 * SLOTS) & MASK:
                    break
                t, t_seq0 = pp, seq0
                i += 2
                continue
            if seq0 != (t_seq0 - SLOTS) & MASK:
                break
            t, t_seq0 = p, seq0
            i += 1
        self.head = head
        self.tail_sect = t
        self.tail_seq = t_seq0
        self.count = (self.next_seq - self.tail_seq) & MASK
        self.state = "ok"

    def read(self, i):
        """LogRing_Read: (READ_OK, запись) или (READ_BAD, None)."""
        sect = (self.tail_sect + i // SLOTS) % self.nsect
        rec = self.slot(sect, i % SLOTS)
        if check(rec) != REC_OK or seq_of(rec) != (self.tail_seq + i) & MASK:
            return READ_BAD, None
        return READ_OK, rec

    def records(self):
        for i in range(self.count):
            yield self.read(i)


# ------------------------------------------------------------- строка CSV

def unpack_meas(rec):
    """LogRecord_UnpackMeas: словарь полей LogMeas."""
    p = rec[PAYLOAD_OFS:PAYLOAD_OFS + PAYLOAD_SIZE]
    m = {}
    for name, ofs, kind in MEAS_FIELDS:
        if kind == "ts":
            t = p[ofs:ofs + 6]
            if t[0] == 0xFF or any(x > 99 for x in t[1:]):
                m["ts"] = TS_UNKNOWN
            else:
                m["ts"] = "%04u-%02u-%02u %02u:%02u:%02u" % ((2000 + t[0],) + tuple(t[1:]))
        elif kind == "f32":
            m[name] = struct.unpack_from("<f", p, ofs)[0]
        elif kind == "u8":
            m[name] = p[ofs]
        elif kind == "u16":
            m[name] = u16(p, ofs)
        elif kind == "u32":
            m[name] = u32(p, ofs)
        elif kind == "raw16":
            v = u16(p, ofs)
            m[name] = float("nan") if v == 0xFFFF else float(v)
        elif kind == "bits":
            bits = u16(p, ofs)
            for bn, lo, w in MEAS_BITS:
                m[bn] = (bits >> lo) & ((1 << w) - 1)
    return m


def cfloat(x, prec):
    """printf("%.Nf") glibc/newlib: знак nan по знаковому биту, inf словом."""
    if math.isnan(x):
        return "-nan" if math.copysign(1.0, x) < 0 else "nan"
    if math.isinf(x):
        return "-inf" if x < 0 else "inf"
    return "%.*f" % (prec, x)


def format_meas(m):
    """LogRecord_FormatMeas: та же строка, с одним \\n."""
    nan = float("nan")
    v = m["bat_valid"]
    fields = [
        m["ts"],
        cfloat(m["air_temp"], 2), cfloat(m["air_hum"], 2),
        cfloat(m["soil_temp"], 2), cfloat(m["soil_hum"], 2),
        cfloat(float(m["bat_percent"]) if v else nan, 0),
        cfloat(m["bat_volts"] if v else nan, 2),
        cfloat(m["vdd"], 3),
        "%u" % (1 if v and not m["bat_low"] else 0),
        "%u" % m["lse_ok"], "%u" % m["time_ok"],
        "%d" % m["aht_init_st"], "%d" % m["aht_read_st"],
        "%u" % m["ds_err"], "%u" % m["air_xchk"], "%u" % m["i2c_rec"],
        "%u" % m["sens_off"], "%u" % m["write_fails"], "%u" % m["lowpower_skips"],
        "%u" % m["reset_cause"], "%u" % m["boot_count"],
        cfloat(m["soil_raw"], 0), cfloat(m["bat_raw"], 0),
    ]
    return ",".join(fields) + "\n"


def format_csv(rec):
    """LogRecord_FormatCsv: строка замера или None (не замер)."""
    if rec[0] != TYPE_MEAS:
        return None
    return format_meas(unpack_meas(rec))


def record_text(rec):
    """LogRecord_Text: байты текста старого архива или b""."""
    n = rec[PAYLOAD_OFS + 1]
    if rec[0] != TYPE_TEXT or n == 0 or n > TEXT_MAX:
        return b""
    return rec[PAYLOAD_OFS + 2:PAYLOAD_OFS + 2 + n]


# ---------------------------------------------------------- файлы тома

class Volume:
    """Содержимое data.txt и data_old.txt, как их собирает VFat_Scan, и
    сведения для сводки (как в info.txt тома)."""

    def __init__(self, journal):
        self.journal = journal
        self.n_meas = self.n_bad = self.n_skip = self.n_text = self.n_import = 0
        self.imp_files = self.imp_dropped = 0
        self.first_ts = self.last_ts = None
        self.last = None
        self.newest_type = None
        data = [(CSV_HEADER + "\r\n").encode("ascii")]
        old = []
        for st, rec in journal.records():
            if st == READ_BAD:
                self.n_bad += 1
                continue
            self.newest_type = rec[0]
            if rec[0] == TYPE_MEAS:
                line = format_csv(rec)
                if len(line) >= LINE_LIMIT:
                    self.n_skip += 1
                    continue
                data.append((line[:-1] + "\r\n").encode("ascii"))
                m = unpack_meas(rec)
                if self.n_meas == 0:
                    self.first_ts = m["ts"]
                self.n_meas += 1
                self.last = m
            else:
                self.n_import += 1
                if rec[0] == TYPE_TEXT:
                    old.append(record_text(rec))
                    self.n_text += 1
                elif rec[0] == TYPE_IMPORT_END:
                    self.imp_files = rec[PAYLOAD_OFS]
                    self.imp_dropped = rec[PAYLOAD_OFS + 5]
        if self.last:
            self.last_ts = self.last["ts"]
        self.data = b"".join(data)
        self.old = b"".join(old)

    def import_pending(self):
        """Импорт старого архива начат и не закончен (LogImport_Pending): том
        в таком состоянии data.txt не отдаёт, прошивка повторит импорт."""
        return self.newest_type in (TYPE_IMPORT_FILE, TYPE_TEXT)


def summary(journal, vol):
    j = journal
    out = []
    if j.state == "ok":
        out.append("журнал: %d слотов (seq %d..%d), секторы %d..%d из %d"
                   % (j.count, j.tail_seq, (j.next_seq - 1) & MASK, j.tail_sect, j.head, j.nsect))
        out.append("замеров: %d, первый %s, последний %s" % (vol.n_meas, vol.first_ts or "-",
                                                            vol.last_ts or "-"))
        out.append("испорченных слотов пропущено: %d" % (vol.n_bad + vol.n_skip))
        if vol.n_import:
            out.append("импорт старого архива: %d Б текста (data_old.txt), файлов %d, не влезло %d"
                       % (len(vol.old), vol.imp_files, vol.imp_dropped))
        if vol.last:
            out.append("последние write_fails=%d lowpower_skips=%d boot_count=%d"
                       % (vol.last["write_fails"], vol.last["lowpower_skips"], vol.last["boot_count"]))
    elif j.state == "empty":
        out.append("журнал пуст: флеш стёрта")
    else:
        out.append("журнала нет, а флеш не пуста — скорее всего, старый FAT-том, ещё не"
                   " перенесённый прошивкой (импорт ждёт питания). Архив из него:"
                   " MTOOLS_SKIP_CHECK=1 mcopy -i <образ>@@32256 ::data.txt .")
    return out


def is_image(raw):
    """Образ флеш, а не CSV: кратен сектору и несёт байты, которых в тексте
    не бывает (0xFF стёртой флеш, нули и двоичные поля записей)."""
    return len(raw) >= SECTOR_SIZE and len(raw) % SECTOR_SIZE == 0 and (b"\xff" in raw or b"\0" in raw)


def read_archive(path):
    """Байты CSV для сводок: файл как есть или data.txt, собранный из образа.
    Второй элемент — строки пояснения (для образа) или []."""
    with open(path, "rb") as f:
        raw = f.read()
    if not is_image(raw):
        return raw, []
    j = Journal(raw)
    vol = Volume(j)
    note = ["образ журнала %d Б: " % len(raw) + "; ".join(summary(j, vol))]
    if vol.import_pending():
        note.append("!! импорт старого архива не закончен — прошивка повторит его")
    return vol.data, note


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    with open(argv[0], "rb") as f:
        img = f.read()
    j = Journal(img)
    vol = Volume(j)
    for line in summary(j, vol):
        print("   " + line)
    if vol.import_pending():
        print("   !! импорт старого архива не закончен: том data.txt не отдаёт, прошивка повторит импорт")
    if len(argv) > 1:
        with open(argv[1], "wb") as f:
            f.write(vol.data)
    if len(argv) > 2 and vol.old:
        with open(argv[2], "wb") as f:
            f.write(vol.old)
    return 0 if j.state != "foreign" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
