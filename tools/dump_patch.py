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
Она только читает: архив на флеш при этом не меняется.
"""
import io
import sys

PATH = "Core/Src/main.c"

DECL_ANCHOR = "static uint32_t write_fails = 0;"
DUMP_ANCHOR = "  // 2b) Архив начинается заново в двух случаях:"

DECL = """

/* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ==== */
#define DUMP_MAX    32768U
#define DUMP_WIN    1024U    /* размер одного окна при разреженном снятии */

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
} dump_info_t;
static volatile dump_info_t dump_info;
static uint8_t dump_buf[DUMP_MAX];"""

DUMP = """  /* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ====
   * Стоит строго ДО блока очистки архива ниже: тот удаляет файл при
   * несовпадении заголовка, и после него читать было бы уже нечего. */
  {
      FIL df;
      if (fs_ok && f_open(&df, DATA_FILE, FA_READ) == FR_OK)
      {
          UINT br = 0;
          uint32_t fsize = (uint32_t)f_size(&df);
          dump_info.fsize = fsize;

          if (fsize <= DUMP_MAX)
          {
              /* Влезает целиком — снимаем подряд, без потерь. */
              f_read(&df, dump_buf, DUMP_MAX, &br);
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
                  if (f_lseek(&df, i * step) != FR_OK)
                      break;
                  if (f_read(&df, dump_buf + got, DUMP_WIN, &part) != FR_OK)
                      break;
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

      DUMP_WIPE_HOOK

      /* Питание датчиков в этой точке ещё включено (его снимает MX_GPIO_Init),
       * но ёмкостному датчику нужно время на выход в режим. */
      HAL_GPIO_WritePin(SENSOR_PWR_GPIO_Port, SENSOR_PWR_Pin, GPIO_PIN_RESET);
      HAL_Delay(SENSOR_POWERUP_DELAY_MS);

      float sv = Soil_ReadVoltage();
      dump_info.soil_mv = (sv != sv) ? 0xFFFFFFFFU : (uint32_t)(sv * 1000.0f);

      Battery_Data bd;
      Battery_Read(&bd);
      dump_info.bat_mv  = (uint32_t)(bd.volts * 1000.0f);
      dump_info.vdd_mv  = (uint32_t)(bd.vdd   * 1000.0f);
      dump_info.bat_pct = bd.percent;

      dump_info.magic = 0xD00DFEEDU;
      /* Дальше по main() не идём и в STOP не уходим: пока ядро крутится здесь,
       * SWD остаётся живым и дамп читается без гонки с циклом сна. */
      while (1) { __NOP(); }
  }

"""


# Очистка идёт строго ПОСЛЕ того, как архив уже вычитан в ОЗУ, и только по явной
# просьбе. Так «снять и начать заново» остаётся одним действием: собранное не
# теряется, а на флеш остаётся пустой файл с одним заголовком.
WIPE = """/* Архив уже лежит в ОЗУ — можно удалять. Здесь файл только исчезает:
       * дампер ниже останавливается и до кода, создающего заголовок, не
       * доходит. Заново файл появится при следующей загрузке, когда на плату
       * вернётся рабочая прошивка — она увидит отсутствие файла, создаст его
       * с заголовком и тут же запишет первый замер. */
      f_unlink(DATA_FILE);"""


def main():
    wipe = "--wipe" in sys.argv

    s = io.open(PATH, encoding="utf-8").read()
    if "dump_info" in s:
        sys.exit("main.c уже пропатчен — сначала откатите правку")
    for a in (DECL_ANCHOR, DUMP_ANCHOR):
        if s.count(a) != 1:
            sys.exit("не найден якорь: %s" % a)
    s = s.replace(DECL_ANCHOR, DECL_ANCHOR + DECL, 1)
    s = s.replace(DUMP_ANCHOR, DUMP + DUMP_ANCHOR, 1)
    s = s.replace("DUMP_WIPE_HOOK", WIPE if wipe else "")
    io.open(PATH, "w", encoding="utf-8", newline="\r\n").write(s)
    print("дампер вставлен" + (" (с очисткой архива)" if wipe else ""))


main()
