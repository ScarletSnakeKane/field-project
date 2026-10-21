# -*- coding: utf-8 -*-
"""Временная правка main.c для снятия архива и живых напряжений через отладчик.

Плата отдаёт данные наружу только как USB-накопитель, то есть нужен физически
воткнутый в компьютер кабель. Когда его нет (например, разработка идёт по
одному лишь ST-Link), архив забирается так: прошивается сборка с этой правкой,
она читает файл в ОЗУ и останавливается, а содержимое ОЗУ вычитывается по SWD.

Заодно снимаются сырые показания, которых в CSV нет: напряжение датчика
влажности почвы до пересчёта в проценты, напряжение банки и реальное VDD.
Сырое напряжение почвы важно именно потому, что в CSV его не видно: пересчёт
прижимает результат к нулю, и «датчик в сухом воздухе» выглядит там ровно так
же, как «датчик оторван» — оба дают 0.00 %.

Всё поле кладётся в одну структуру: раскладку отдельных переменных по .bss
линкерволен менять, а у структуры смещения фиксированы, и читать её по SWD
можно одним запросом.

Правка НЕ предназначена для коммита — её накладывает и откатывает read_data.sh.

    python tools/dump_patch.py                       только читает архив
    python tools/dump_patch.py --wipe <размер> <хеш> второй проход очистки
    python tools/dump_patch.py --revert [файл]       убрать вставку из main.c
                                                     (или из его копии)

Очистка — всегда второй, отдельный проход. Первый только читает; хост
сохраняет файл на диск и сверяет его с тем, что было на флеш. Второй дампер
перечитывает файл заново и удаляет его, только если размер и хеш совпали с
сохранённым на хосте, — то есть удаляется ровно то, что уже лежит на диске.
"""
import io
import re
import sys

PATH = "Core/Src/main.c"

DECL_ANCHOR = "static uint32_t write_fails = 0;"
DUMP_ANCHOR = "  // 2b) Архив начинается заново в двух случаях:"

BEGIN = "/* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ===="
END = "/* ==== КОНЕЦ ВРЕМЕННОГО ДАМПЕРА ==== */"

# Раскладку структуры читает read_data.sh по номерам слов — менять только вместе.
DECL = """

""" + BEGIN + """ */
#define DUMP_MAX    32768U
#define DUMP_WIN    1024U    /* размер одного окна при разреженном снятии */

/* Коды status: младший байт — где сломалось, следующий — FRESULT. */
#define DUMP_ST_OK       0U
#define DUMP_ST_NO_FS    1U  /* том не смонтирован */
#define DUMP_ST_OPEN     2U  /* f_open не открыл файл (FR_NO_FILE = 4 — файла нет) */
#define DUMP_ST_READ     3U  /* f_read вернул ошибку */
#define DUMP_ST_SEEK     4U  /* f_lseek вернул ошибку */

/* Коды wipe. */
#define DUMP_WIPE_NONE    0U /* очистку не просили */
#define DUMP_WIPE_DONE    1U /* файл удалён */
#define DUMP_WIPE_REFUSED 2U /* файл не совпал с сохранённым на хосте — не тронут */
#define DUMP_WIPE_FAILED  3U /* f_unlink вернул ошибку (код в старшем байте) */

typedef struct {
    uint32_t magic;      /* 0xD00DFEED — дамп действительно выполнился */
    uint32_t fsize;      /* размер data.txt на флеш */
    uint32_t len;        /* сколько байт реально снято в буфер */
    uint32_t win_bytes;  /* размер окна; 0 — файл снят целиком, подряд */
    uint32_t win_count;  /* сколько окон лежит в буфере подряд */
    uint32_t win_step;   /* шаг между началами окон в файле */
    uint32_t soil_mv;    /* напряжение датчика почвы ДО пересчёта, мВ */
    uint32_t bat_mv;     /* напряжение банки, мВ */
    uint32_t vdd_mv;     /* реальное VDD через VREFINT, мВ */
    uint32_t bat_pct;    /* заряд по кривой, % */
    uint32_t status;     /* DUMP_ST_*: почему снято 0 байт — нет тома, файла или чтения */
    uint32_t hash;       /* FNV-1a по len байтам буфера: хост сверяет, что прочёл то же */
    uint32_t wipe;       /* DUMP_WIPE_* */
} dump_info_t;
static volatile dump_info_t dump_info;
static uint8_t dump_buf[DUMP_MAX];
""" + END

DUMP = "  " + BEGIN + """
   * Стоит строго ДО блока очистки архива ниже: тот удаляет файл при
   * несовпадении заголовка, и после него читать было бы уже нечего. */
  {
      FIL df;
      FRESULT dr;
      if (!fs_ok)
      {
          dump_info.status = DUMP_ST_NO_FS;
      }
      else if ((dr = f_open(&df, DATA_FILE, FA_READ)) != FR_OK)
      {
          dump_info.status = DUMP_ST_OPEN | ((uint32_t)dr << 8);
      }
      else
      {
          UINT br = 0;
          uint32_t fsize = (uint32_t)f_size(&df);
          dump_info.fsize = fsize;

          if (fsize <= DUMP_MAX)
          {
              /* Влезает целиком — снимаем подряд, без потерь. */
              dr = f_read(&df, dump_buf, DUMP_MAX, &br);
              if (dr != FR_OK)
                  dump_info.status = DUMP_ST_READ | ((uint32_t)dr << 8);
              dump_info.len = (uint32_t)br;
          }
          else
          {
              /* Не влезает. Хвост брать нельзя: за сутки архив перерастает буфер
               * в несколько раз, и по одному хвосту не увидеть ни тренда батареи,
               * ни того, что происходило ночью. Поэтому берём окна, равномерно
               * разложенные по всей длине файла: каждое — десяток целых строк,
               * а вместе они покрывают весь период. Первое окно начинается с
               * нуля, так что заголовок всегда на месте. */
              const uint32_t count = DUMP_MAX / DUMP_WIN;
              const uint32_t step  = (fsize - DUMP_WIN) / (count - 1);
              uint32_t got = 0;

              for (uint32_t i = 0; i < count; i++)
              {
                  UINT part = 0;
                  if ((dr = f_lseek(&df, i * step)) != FR_OK)
                  {
                      dump_info.status = DUMP_ST_SEEK | ((uint32_t)dr << 8);
                      break;
                  }
                  if ((dr = f_read(&df, dump_buf + got, DUMP_WIN, &part)) != FR_OK)
                  {
                      dump_info.status = DUMP_ST_READ | ((uint32_t)dr << 8);
                      break;
                  }
                  got += part;
                  if (part < DUMP_WIN)
                      break;
              }

              dump_info.len       = got;
              dump_info.win_bytes = DUMP_WIN;
              dump_info.win_count = count;
              dump_info.win_step  = step;
              br = got;
          }
          f_close(&df);
      }

      {
          uint32_t h = 2166136261U;
          for (uint32_t i = 0; i < dump_info.len; i++)
          {
              h ^= dump_buf[i];
              h *= 16777619U;
          }
          dump_info.hash = h;
      }

      DUMP_WIPE_HOOK

      /* Питание датчиков в этой точке ещё включено (его снимает MX_GPIO_Init),
       * но ёмкостному датчику нужно время на выход в режим. */
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
      /* Дальше по main() не идём и в STOP не уходим: пока ядро крутится здесь,
       * SWD остаётся живым и дамп читается без гонки с циклом сна. */
      while (1) { __NOP(); }
  }
  """ + END + """

"""


# Удаляется только файл, который хост уже сохранил целиком: снят подряд (не
# окнами), прочитан без ошибок, и размер с хешем совпали с переданными с хоста.
# Раньше удаление стояло безусловно сразу после чтения — и стирало весь архив,
# даже когда в буфер попали лишь окна по 1 КБ, f_read вернул ошибку или хост
# так и не дочитал ОЗУ (D1).
WIPE = """/* Здесь файл только исчезает: дампер ниже останавливается и до кода,
       * создающего заголовок, не доходит. Заново файл появится при следующей
       * загрузке рабочей прошивки — с заголовком и первым замером. */
      if (dump_info.status == DUMP_ST_OK && dump_info.win_bytes == 0U
          && dump_info.len == dump_info.fsize
          && dump_info.fsize == %(size)uU && dump_info.hash == 0x%(hash)08XU)
      {
          FRESULT ur = f_unlink(DATA_FILE);
          dump_info.wipe = (ur == FR_OK) ? DUMP_WIPE_DONE
                                         : (DUMP_WIPE_FAILED | ((uint32_t)ur << 8));
      }
      else
      {
          dump_info.wipe = DUMP_WIPE_REFUSED;
      }"""


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
            sys.exit("--wipe требует размер и хеш сохранённого на хосте файла")
        wipe = {"size": int(args[1]), "hash": int(args[2], 0)}
    elif args:
        sys.exit("неизвестные аргументы: %s" % " ".join(args))

    s = read_src()
    if "dump_info" in s:
        sys.exit("main.c уже пропатчен — сначала откатите правку")
    for a in (DECL_ANCHOR, DUMP_ANCHOR):
        if s.count(a) != 1:
            sys.exit("не найден якорь: %s" % a)

    hook = WIPE % wipe if wipe else ""
    nl = eol_of(s)
    decl = DECL.replace("\n", nl)
    dump = DUMP.replace("DUMP_WIPE_HOOK", hook).replace("\n", nl)
    s = s.replace(DECL_ANCHOR, DECL_ANCHOR + decl, 1)
    s = s.replace(DUMP_ANCHOR, dump + DUMP_ANCHOR, 1)
    write_src(s)
    print("дампер вставлен" + (" (очистка, если файл совпадёт с сохранённым)" if wipe else ""))


if __name__ == "__main__":
    main()
