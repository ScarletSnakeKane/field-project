# -*- coding: utf-8 -*-
"""Временная правка main.c: снять сырой образ журнала и живые напряжения по SWD.

Обычный путь снять архив — USB самой платы: том FIELDSENSOR отдаёт data.txt
целиком (read_data.sh без ключей просто копирует его). Дампер нужен, когда
USB нет или он не поднимается, для диагностики самой флеш и для --wipe.

Архив на флеш — кольцевой журнал двоичных записей (Core/Inc/log_ring.h),
файловой системы там больше нет. Дампер встаёт перед открытием журнала в
main(), то есть до всего, что могло бы писать во флеш (импорт старого тома,
стирание запасного сектора), и дальше по main() не идёт: флеш снимается
такой, какой её оставила рабочая прошивка. Он
  1. считает FNV-1a по всей флеш (flash_hash, hash_ok) — хост сверит с ним
     собранный образ целиком;
  2. только в проходе --wipe: стирает всю флеш, если её размер и хеш
     совпали с образом, сохранённым на хосте (см. ниже);
  3. снимает живые напряжения почвы и банки (в CSV их нет в момент снятия);
  4. ставит magic и отдаёт флеш кусками по 32 КБ по запросу хоста, пиная
     сторожа (dump_serve_raw). Куски на диске собирает unpack_dump.py --raw,
     CSV из образа — tools/log_image.py.

Всё поле кладётся в одну структуру: раскладку отдельных переменных по .bss
линкер волен менять, а у структуры смещения фиксированы, и читать её по SWD
можно одним запросом. Слова dump_info_t (по 4 байта, номер — смещение / 4):
    0 magic 0xD00DFEED   1 raw_total   2 soil_mv   3 bat_mv   4 vdd_mv
    5 bat_pct   6 flash_hash   7 hash_ok   8 wipe   9 raw_req (0x24)
    10 raw_done (0x28)   11 raw_off   12 raw_len   13 raw_hash

Правка НЕ предназначена для коммита — её накладывает и откатывает read_data.sh.

    python tools/dump_patch.py                       только снимает
    python tools/dump_patch.py --wipe <размер> <хеш> второй проход: стереть
    python tools/dump_patch.py --revert [файл]       убрать вставку из main.c
                                                     (или из его копии)

Кусок N (0..), вручную. Все команды программатора — с «-c port=SWD
mode=HOTPLUG»: режим по умолчанию сбрасывает плату, дампер стартует заново,
и запрос пропадает.
    -w32 <адрес dump_info + 0x24> N+1     запросить кусок
    -r32 <адрес dump_info> 56             ждать raw_done == N+1; raw_off, raw_len, raw_hash
    -r <адрес dump_buf> <raw_len> <файл>  забрать байты
    python tools/unpack_dump.py --raw <файл> build/flash.img <raw_off> <raw_hash>
    python tools/log_image.py build/flash.img build/data.txt build/data_old.txt
Слова -r32 печатает шестнадцатеричными без 0x — unpack_dump.py --raw так их
и принимает. raw_len = 0 — кусок за краем или чтение не удалось. Повторная
запись того же N+1 ничего не делает: чтобы перечитать кусок, сначала
записать 0 в raw_done (+0x28). read_data.sh --swd проходит всё это сам.

Стирание — всегда второй, отдельный проход. Первый только читает; хост
собирает образ, сверяет его с flash_hash, разбирает журнал без ошибок и
кладёт несменяемую копию. Второй дампер заново считает хеш всей флеш и
стирает её, только если размер и хеш совпали с переданными с хоста, — то
есть стирается ровно то, что уже лежит на диске. Каждый сектор проверяется
чтением после стирания. Рабочая прошивка на стёртой флеш начинает журнал с
нуля.
"""
import io
import re
import sys

PATH = "Core/Src/main.c"

DECL_ANCHOR = "static uint32_t write_fails = 0;"
# Строка открытия журнала при старте: дампер встаёт перед ней (вставка —
# с начала этой строки). Ищется по коду, а не по комментарию.
DUMP_ANCHOR_RE = r"^[ \t]*fs_ok = Archive_Open\(boot_power_ok\);"

BEGIN = "/* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ===="
END = "/* ==== КОНЕЦ ВРЕМЕННОГО ДАМПЕРА ==== */"

# Раскладку структуры читает read_data.sh по номерам слов — менять только вместе.
DECL = """

""" + BEGIN + """ */
#define DUMP_MAX    32768U

/* Коды wipe: младший байт — итог, следующие — сколько секторов не стёрлось. */
#define DUMP_WIPE_NONE    0U /* стирание не просили */
#define DUMP_WIPE_DONE    1U /* вся флеш стёрта и проверена чтением */
#define DUMP_WIPE_REFUSED 2U /* флеш не та, что сохранена на хосте, — не тронута */
#define DUMP_WIPE_FAILED  3U /* защита не снялась или секторы не стёрлись */
#define DUMP_WIPE_NOREAD  4U /* флеш не прочиталась при сверке — не тронута */

typedef struct {
    uint32_t magic;      /* 0xD00DFEED — дамп действительно выполнился */
    uint32_t raw_total;  /* размер флеш, байт */
    uint32_t soil_mv;    /* напряжение датчика почвы ДО пересчёта, мВ */
    uint32_t bat_mv;     /* напряжение банки, мВ */
    uint32_t vdd_mv;     /* реальное VDD через VREFINT, мВ */
    uint32_t bat_pct;    /* заряд по кривой, % */
    uint32_t flash_hash; /* FNV-1a по всей флеш */
    uint32_t hash_ok;    /* 1 — вся флеш прочиталась при подсчёте хеша */
    uint32_t wipe;       /* DUMP_WIPE_* */
    uint32_t raw_req;    /* хост: номер куска + 1 (0 — ничего не надо) */
    uint32_t raw_done;   /* дампер: какой запрос выполнен; хост пишет 0, чтобы повторить */
    uint32_t raw_off;    /* смещение куска на флеш */
    uint32_t raw_len;    /* сколько байт куска в буфере; 0 — за краем или отказ */
    uint32_t raw_hash;   /* FNV-1a по raw_len байтам буфера */
} dump_info_t;
static volatile dump_info_t dump_info;
static uint8_t dump_buf[DUMP_MAX];

static uint32_t dump_fnv(uint32_t h, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

/* Хеш всей флеш порциями по 4 КБ: у одной транзакции SPI таймаут 100 мс
 * на всю длину, а мегабайты побайтовым опросом идут секунды. Между
 * порциями — пинок сторожу (запущен с первой строки main, ~22–32 с). */
static void dump_hash_flash(void)
{
    uint32_t h = 2166136261U;
    dump_info.hash_ok = 0;
    for (uint32_t off = 0; off < (uint32_t)FLASH_SIZE_BYTES; off += 4096U)
    {
        IWDG->KR = 0x0000AAAAU;
        if (!SPI_Flash_Read(off, dump_buf, 4096U))
            return;
        h = dump_fnv(h, dump_buf, 4096U);
    }
    dump_info.flash_hash = h;
    dump_info.hash_ok = 1;
}

/* Стереть всю флеш и проверить чтением каждый сектор: запертая SST26
 * отвечает на стирание «успехом», ничего не стерев (T2 отчёта П5а). */
static void dump_wipe(uint32_t size, uint32_t hash)
{
    if (!dump_info.hash_ok)
    {
        dump_info.wipe = DUMP_WIPE_NOREAD;
        return;
    }
    if (dump_info.raw_total != size || dump_info.flash_hash != hash)
    {
        dump_info.wipe = DUMP_WIPE_REFUSED;
        return;
    }
    if (!SPI_Flash_Unlock())
    {
        dump_info.wipe = DUMP_WIPE_FAILED;
        return;
    }
    uint32_t bad = 0;
    for (uint32_t off = 0; off < (uint32_t)FLASH_SIZE_BYTES; off += 4096U)
    {
        IWDG->KR = 0x0000AAAAU;
        uint8_t ok = SPI_Flash_EraseSector(off);   /* у SST26 до 25 мс */
        IWDG->KR = 0x0000AAAAU;
        if (ok)
            ok = SPI_Flash_Read(off, dump_buf, 4096U);
        for (uint32_t i = 0; ok && i < 4096U; i++)
            if (dump_buf[i] != 0xFFU)
                ok = 0;
        if (!ok)
            bad++;
    }
    dump_info.wipe = bad ? (DUMP_WIPE_FAILED | (bad << 8)) : DUMP_WIPE_DONE;
}

/* Один запрос сырого куска. Отдельной функцией — её зовёт и самопроверка
 * (tools/test_tools.py) без вечного цикла. */
static void dump_raw_step(void)
{
    uint32_t req = dump_info.raw_req;
    if (req == 0U || req == dump_info.raw_done)
        return;

    uint32_t off = (req - 1U) * DUMP_MAX;
    uint32_t n = 0;
    if (off < (uint32_t)FLASH_SIZE_BYTES)
    {
        n = (uint32_t)FLASH_SIZE_BYTES - off;
        if (n > DUMP_MAX)
            n = DUMP_MAX;
        for (uint32_t done = 0; done < n; done += 4096U)
        {
            uint32_t part = (n - done > 4096U) ? 4096U : n - done;
            IWDG->KR = 0x0000AAAAU;
            if (!SPI_Flash_Read(off + done, dump_buf + done, part))
            {
                n = 0;
                break;
            }
        }
    }

    dump_info.raw_off  = off;
    dump_info.raw_len  = n;
    dump_info.raw_hash = dump_fnv(2166136261U, dump_buf, n);
    dump_info.raw_done = req;   /* последним: по нему хост понимает, что готово */
}

/* Дальше по main() не идём и в STOP не уходим: пока ядро крутится здесь,
 * SWD остаётся живым и образ читается без гонки с циклом сна, а журнал не
 * открывается и ничего во флеш не пишет. Сторож кормится в цикле. */
static void dump_serve_raw(void)
{
    for (;;)
    {
        IWDG->KR = 0x0000AAAAU;
        dump_raw_step();
    }
}
""" + END

DUMP = "  " + BEGIN + """
   * Стоит строго ДО открытия журнала: оно на первом старте переносит старый
   * том (пишет), а первая запись стирает запасной сектор. Снимается флеш
   * такой, какой её оставила рабочая прошивка. */
  {
      dump_info.raw_total = (uint32_t)FLASH_SIZE_BYTES;
      dump_hash_flash();
      DUMP_WIPE_HOOK

      /* Питание датчиков в этой точке включено (выше по main), но
       * ёмкостному датчику нужно время на выход в режим. */
      HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);
      HAL_Delay(SENSOR_POWERUP_DELAY_MS);

      float sv = Soil_ReadVoltage();
      dump_info.soil_mv = (sv != sv) ? 0xFFFFFFFFU : (uint32_t)(sv * 1000.0f);

      /* При отказе замера volts/vdd = NAN, а percent = 255: приведение NAN к
       * uint32_t не определено (на M4 даёт 0), и хост показывал «0 мВ (255%)»,
       * то есть пустую банку. Отказ помечаем так же, как у почвы. */
      Battery_Data bd;
      Battery_Read(&bd);
      if (bd.valid)
      {
          dump_info.bat_mv  = (uint32_t)(bd.volts * 1000.0f);
          dump_info.vdd_mv  = (uint32_t)(bd.vdd   * 1000.0f);
          dump_info.bat_pct = bd.percent;
      }
      else
      {
          dump_info.bat_mv = dump_info.vdd_mv = dump_info.bat_pct = 0xFFFFFFFFU;
      }

      dump_info.magic = 0xD00DFEEDU;
      dump_serve_raw();
  }
  """ + END + """

"""


# Стирается только флеш, образ которой хост уже сохранил целиком: дампер
# второго прохода сам пересчитывает хеш всей флеш и сверяет его и размер с
# вписанными сюда значениями сохранённого образа (D1 в новом виде: раньше
# так же сверялся файл перед f_unlink).
WIPE = """/* Стереть, только если флеш — ровно сохранённый на хосте образ. */
      dump_wipe(%(size)uU, 0x%(hash)08XU);"""


def read_src(path=PATH):
    # newline="" — читать байт-в-байт, не трогая CRLF: иначе запись обратно
    # перевела бы все строки в одно окончание, и git diff показал бы изменённым
    # весь файл, а вставка в нём потерялась бы (D5).
    return io.open(path, encoding="utf-8", newline="").read()


def write_src(s, path=PATH):
    io.open(path, "w", encoding="utf-8", newline="").write(s)


def eol_of(s):
    return "\r\n" if "\r\n" in s else "\n"


def revert(s):
    """Убрать вставку, не трогая остального: правки, сделанные в main.c за время
    снятия, при этом сохраняются."""
    b, e = re.escape(BEGIN), re.escape(END)
    s, n1 = re.subn(r"(?:\r?\n){2}" + b + r".*?" + e, "", s, count=1, flags=re.S)
    s, n2 = re.subn(r"  " + b + r".*?" + e + r"(?:\r?\n){2}", "", s, count=1, flags=re.S)
    return s, n1 + n2


def main():
    args = sys.argv[1:]

    if args[:1] == ["--revert"]:
        # Необязательный путь — копия main.c с правками владельца (read_data.sh).
        path = args[1] if len(args) > 1 else PATH
        s, n = revert(read_src(path))
        if BEGIN in s or "dump_info" in s:
            sys.exit("вставка дампера убрана не полностью — откатите %s руками" % path)
        if n:
            write_src(s, path)
        print("дампер убран" if n else "дампера в main.c нет")
        return

    wipe = None
    if args[:1] == ["--wipe"]:
        if len(args) != 3:
            sys.exit("--wipe требует размер и хеш сохранённого на хосте образа")
        wipe = {"size": int(args[1]), "hash": int(args[2], 0)}
    elif args:
        sys.exit("неизвестные аргументы: %s" % " ".join(args))

    s = read_src()
    if "dump_info" in s:
        sys.exit("main.c уже пропатчен — сначала откатите правку")
    if s.count(DECL_ANCHOR) != 1:
        sys.exit("не найден якорь: %s" % DECL_ANCHOR)
    spots = list(re.finditer(DUMP_ANCHOR_RE, s, re.M))
    if len(spots) != 1:
        sys.exit("не найден якорь (или найден не один): %s" % DUMP_ANCHOR_RE)
    at = spots[0].start()

    # Без --wipe функция стирания не вызывается, но упоминается: иначе сборка
    # с -Wall предупреждает о неиспользуемой static-функции.
    hook = WIPE % wipe if wipe else "(void)dump_wipe;   /* стирание не просили */"
    nl = eol_of(s)
    decl = DECL.replace("\n", nl)
    dump = DUMP.replace("DUMP_WIPE_HOOK", hook).replace("\n", nl)
    s = s[:at] + dump + s[at:]
    s = s.replace(DECL_ANCHOR, DECL_ANCHOR + decl, 1)
    write_src(s)
    print("дампер вставлен" + (" (стирание, если флеш совпадёт с сохранённым образом)" if wipe else ""))


if __name__ == "__main__":
    main()
