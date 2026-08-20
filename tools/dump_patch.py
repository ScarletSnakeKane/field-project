# -*- coding: utf-8 -*-
"""Временная правка main.c для снятия архива через отладчик.

Плата отдаёт данные наружу только как USB-накопитель, то есть нужен физически
воткнутый в компьютер кабель. Когда его нет (например, разработка идёт по
одному лишь ST-Link), архив забирается так: прошивается сборка с этой правкой,
она читает файл в ОЗУ и останавливается, а содержимое ОЗУ вычитывается по SWD.

Правка НЕ предназначена для коммита — её накладывает и откатывает read_data.sh.
Она только читает: архив на флеш при этом не меняется.
"""
import io, sys

PATH = "Core/Src/main.c"

DECL_ANCHOR = "static uint32_t write_fails = 0;"
DUMP_ANCHOR = "  // 2b) Архив начинается заново в двух случаях:"

DECL = """

/* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ==== */
#define DUMP_MAX  32768U
static uint8_t  dump_buf[DUMP_MAX];
static volatile uint32_t dump_magic = 0;
static volatile uint32_t dump_fsize = 0;
static volatile uint32_t dump_len   = 0;"""

DUMP = """  /* ==== ВРЕМЕННЫЙ ДАМПЕР (tools/dump_patch.py) — НЕ КОММИТИТЬ ====
   * Стоит строго ДО блока очистки архива ниже: тот удаляет файл при
   * несовпадении заголовка, и после него читать было бы уже нечего. */
  {
      FIL df;
      if (fs_ok && f_open(&df, DATA_FILE, FA_READ) == FR_OK)
      {
          UINT br = 0;
          dump_fsize = (uint32_t)f_size(&df);
          if (dump_fsize > DUMP_MAX)
              f_lseek(&df, dump_fsize - DUMP_MAX);   /* не влезло — берём хвост */
          f_read(&df, dump_buf, DUMP_MAX, &br);
          dump_len = (uint32_t)br;
          f_close(&df);
      }
      dump_magic = 0xD00DFEEDU;
      /* Дальше по main() не идём и в STOP не уходим: пока ядро крутится здесь,
       * SWD остаётся живым и дамп читается без гонки с циклом сна. */
      while (1) { __NOP(); }
  }

"""

def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "dump_magic" in s:
        sys.exit("main.c уже пропатчен — сначала откатите правку")
    for a in (DECL_ANCHOR, DUMP_ANCHOR):
        if s.count(a) != 1:
            sys.exit("не найден якорь: %s" % a)
    s = s.replace(DECL_ANCHOR, DECL_ANCHOR + DECL, 1)
    s = s.replace(DUMP_ANCHOR, DUMP + DUMP_ANCHOR, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\r\n").write(s)
    print("дампер вставлен")

main()
