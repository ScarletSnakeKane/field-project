# Отчёт П5в — инструменты под журнал

Ветка `fix/p5c-tools` от `48897d5` (П5а и П5б слиты). Владение: `tools/` (кроме `syncheck.sh`),
Python-тесты инструментов (`tools/test_tools.py`), `.claude/skills/fieldsensor/SKILL.md`.
Прошивку (`Core/`, `USB_DEVICE/`, `Middlewares/`, `tests/`) не трогал.

| Пункт брифа | Статус | Коммит | Тест / мутация |
|---|---|---|---|
| 1. Дампер и `read_data.sh` без FatFs: сырой образ журнала по SWD порциями с пинком сторожа | сделано | `e241cf2` | `Dumper.*`, `ReadDataBench.test_swd_*`; M11–M14, M17, M22 |
| 1. `unpack_dump.py` / разбор записей → тот же CSV, что том | сделано | `ae95eab` | `LogImage.*` (10 сценариев против `log_ring.h`+`vfat.h`); M1–M8, M18, M20 |
| 1. Формат записи — таблица полей с тестом на соответствие `log_record.h` | сделано | `ae95eab`, `e241cf2` | `LogFormat.*` (без gcc), `LogImage.*` (с gcc); M5, M9 |
| 2. `--wipe`: только после сверки, что образ на диске и разобран без ошибок | сделано | `e241cf2` | `Dumper.test_wipe_*`, `ReadDataBench.test_wipe_*`; M11–M14, M22 |
| 3. `summarize_data.py` / `analyze_run.py`: CSV с тома и образ | сделано | `ae95eab` | `LogImage.test_summaries_read_image`; прежние `NanRanges`, `NewColumns`, `BatteryCurve` |
| 4. `test_tools.py`: образ модели журнала → CSV = эталон; `--wipe` отказывается при неполном образе | сделано | `e241cf2` | `LogImage.*`, `test_unpack_image`, `test_wipe_refused_on_incomplete_image` |
| 5. SKILL.md: новое устройство архива | сделано | `982f005` | — |
| Владелец: `read_data.sh` по умолчанию копирует `data.txt` с тома USB | сделано | `e241cf2` | `ReadDataBench.test_usb_*`, `UnpackUsb.*`; M19, M21 |
| 22 FAIL + 2 ERROR прежнего `test_tools.py` | все переписаны под журнал | `e241cf2` | см. «Упавшие тесты» ниже |
| Замечания инспектора (БЛОК, ВАЖНО, мелочи) | закрыты | `c7130d1` | `test_usb_found_among_drives`, `test_usb_default_roots_are_drive_letters`, `test_wipe_refused_on_pending_import` |

## Как теперь снимается архив

| Путь | Кому | Что делает |
|---|---|---|
| Том USB `FIELDSENSOR` | заказчику и владельцу | скопировать `data.txt`. `./tools/read_data.sh` без ключей делает то же самое и сверяет копию с `info.txt` того же тома |
| `./tools/read_data.sh --swd` | владельцу: нет USB, на томе `ARCHIVE NOT AVAILABLE`, диагностика флеш | дампер, образ всей флеш (`build/data.img`), из него `data.txt`/`data_old.txt` — байт в байт как на томе |
| `./tools/read_data.sh --wipe` | только владельцу | `--swd`, затем второй дампер стирает флеш по сверке |

### USB (по умолчанию)

- Скрипт ждёт диск до `USB_WAIT_SEC` (60 с; том собирается 5–20 с). Где искать:
  - явно — `FS_VOLUME`;
  - иначе — буквы дисков `/d`…`/z` перебором (маска `/[d-z]` в Git Bash не раскрывается, R7),
    `/media/*/*`, `/run/media/*/*`, `/Volumes/*`; свой список — `FS_ROOTS`.
- Том узнаётся по первой строке `info.txt`: `FieldSensor `.
- Копирование идёт в `data.txt.part`. `unpack_dump.py --usb` сверяет:
  - заголовок — тот, что в `log_record.h`;
  - файл кончается целой строкой;
  - число строк и последняя метка — как в `info.txt`.
- Не сошлось — код 1, на место архива ничего не ложится, прошлый снимок остаётся в `.prev`.
- Рядом кладутся `<файл>_old.txt` (если на томе есть `data_old.txt`) и `<файл>_info.txt`.
- Программатор и сборка не нужны.

### SWD (`--swd`)

- Дампер встаёт в `main()` перед `fs_ok = Archive_Open(boot_power_ok);` — якорь ищется по коду.
  - После `SPI_Flash_Init`, но до всего, что пишет во флеш: импорт старого тома, стирание
    запасного первой записью.
  - Дальше по `main()` не идёт: образ — флеш такой, какой её оставила рабочая прошивка.
- Порядок работы дампера:
  1. FNV-1a всей флеш (порциями по 4 КБ, пинок сторожу на каждой). Итог — `flash_hash`/`hash_ok`.
  2. Живые напряжения почвы и банки.
  3. `magic`.
  4. Раздача кусков по 32 КБ по `raw_req`. Повтор того же куска — после записи 0 в `raw_done`.
- `dump_info_t` — 14 слов: `magic, raw_total, soil_mv, bat_mv, vdd_mv, bat_pct, flash_hash,
  hash_ok, wipe, raw_req (0x24), raw_done (0x28), raw_off, raw_len, raw_hash`.
- Хост на каждый кусок:
  1. `-w32` обнуляет `raw_done` и пишет `raw_req`.
  2. Ждёт `raw_done == N` по `-r32` (вывод `-w32` не разбирается: у версий программатора он разный).
  3. Проверяет, что `raw_off` — запрошенное смещение, читает `raw_len` байт и сверяет хеш куска
     (`unpack_dump.py --raw`).
  4. Три попытки на кусок.
- Образ собирается в `build/data.img.part`. Только полный образ становится `build/data.img`.
- `unpack_dump.py --image` проверяет размер (ровно `raw_total`) и хеш всей флеш, разбирает журнал
  и пишет `data.txt` / `data_old.txt`.
- Код ≠ 0, если:
  - образ неполный;
  - хеш не тот;
  - флеш чужая (старый FAT-том, ещё не перенесённый);
  - импорт старого архива не закончен.
- Время на плате (оценка): 64 куска × 4–5 вызовов программатора по ~1 с — 4–6 минут на 2 МБ.
  Хеш флеш на старте дампера — ~5 с.

### `--wipe`

Второй проход, только если:
- все куски сняты;
- `hash_ok`;
- образ на диске сошёлся с `flash_hash`;
- журнал разобран с кодом 0;
- в `build/archive/` легли несменяемые копии `flash-<дата>-<хеш>.img`, `data-<дата>-<хеш>.txt`
  (и `data_old-…`), копия образа сверена по хешу и снята с записи.

Второй дампер:
1. Заново считает хеш всей флеш.
2. Если размер и хеш совпали с вписанными в патч — снимает защиту, стирает все 512 секторов и
   проверяет каждый чтением. Запертая SST26 отвечает на стирание «успехом» (T2 П5а).
3. Итог в `wipe`:
   - 1 — стёрто;
   - 2 — флеш не та;
   - 3 — защита не снялась или N секторов не стёрлись;
   - 4 — флеш не прочиталась при сверке.

Рабочая прошивка на стёртой флеш начинает журнал с сектора 0 (`LOG_MOUNT_EMPTY` → `LOG_OPEN_OK`).

## Разбор образа (`tools/log_image.py`)

- `Journal`: порт `LogRing_SectorSeq` и `LogRing_Mount`:
  - голова — наибольший seq по модулю 2³²;
  - позиция записи — за последним нестёртым слотом головы;
  - хвост — назад, пока номера смежны;
  - мост через сектор без годного номера, если за ним номер ровно −128;
  - запасной и сектор записи в журнал не входят.
- Чтение — как `LogRing_Read`: CRC, тип, версия и seq слота.
- `Volume`: как `VFat_Scan`:
  - заголовок + `\r\n`;
  - замеры строкой `LogRecord_FormatMeas` с `\r\n`;
  - испорченные слоты пропускаются;
  - строка ≥ 447 Б выпадает, как у тома (`VFAT_LINE_BUF − 1`);
  - `T` склеиваются в `data_old.txt`;
  - сведения как в `info.txt`.
- float печатается `"%.Nf"` Python (корректное округление, как glibc/newlib). Знак `nan` берётся по
  знаковому биту, `inf` печатается словом — как у printf.
- Таблица полей `MEAS_FIELDS`/`MEAS_BITS` и константы сверяются с `log_record.h` двумя способами.
  1. **Текстом** (`LogFormat`, работает и под Windows без gcc):
     - смещения и виды полей — из `LogRecord_PackMeas`;
     - битовые поля — из выражения `bits`;
     - `#define` размеров и типов, `LOG_CSV_HEADER`, `LOG_TS_UNKNOWN`, `VFAT_LINE_BUF`;
     - CRC32 IEEE.
  2. **Байт в байт** (`LogImage`, где есть gcc): `tools/log_oracle.c` собирается с настоящими
     `log_ring.h` и `vfat.h`. Журнал пишется `LogRing_Append` на модели NOR, файлы читаются
     секторами тома через `VFat_Read`. Python должен получить из образа то же.
- Сценарии оракула: пустая флеш; 150 записей; два круга кольца; переход seq через 2³²;
  испорченные слоты, в том числе:
  - сектор нулей (мост);
  - бит типа слота 0 (T3);
  - годная запись не на своём месте;
  - перезапуски с пропуском (T4);
  - недописанная последняя запись.

  Ещё сценарии: журнал, начатый заново вплотную за старыми номерами; оборванное стирание
  запасного; импорт с `E`; незаконченный импорт; чужая флеш.
- Замеры в оракуле случайные:
  - любые битовые образы float, nan, −nan, ±inf, −0, субнормальные;
  - насыщение кодов, неразобранная метка, сырые отсчёты за краем.
- Если формат записи в прошивке поменяется, упадёт `LogFormat`, а с gcc — ещё и `LogImage`.

## Упавшие тесты прежнего `test_tools.py` — что с каждым

| Было | Стало |
|---|---|
| `HeaderColumns.test_new_columns_fixture_is_current`, `test_parser_knows_columns` (ERROR) | заголовок берётся из `LOG_CSV_HEADER` в `log_record.h`; добавлен `test_main_uses_log_header` (`CSV_HEADER` в `main.c` — синоним) |
| `HeaderColumns.test_format_matches_header` | число преобразований в `snprintf` `LogRecord_FormatMeas` = числу колонок |
| `DumperWipe.test_read_only_never_unlinks` | `Dumper.test_read_only_never_erases`: без `--wipe` ни одного стирания, флеш не тронута |
| `DumperWipe.test_wipe_when_matches` | `Dumper.test_wipe_when_matches`: стёрта вся флеш |
| `test_wipe_refused_on_hash_mismatch`, `_on_size_change` | `test_wipe_refused_on_hash_mismatch`, `_on_size_mismatch` |
| `test_wipe_refused_on_read_error`, `_full_buffer` | `test_wipe_refused_on_read_error`: сбой чтения при подсчёте хеша — `hash_ok = 0`, отказ. Случай «буфер полон, но статус — ошибка» ушёл вместе с `f_read`: теперь любой отказ чтения обнуляет `hash_ok` |
| `test_wipe_refused_when_windowed` | окон больше нет (дампер отдаёт всю флеш). Смысл «стирать только целиком снятое» — `ReadDataBench.test_wipe_refused_on_incomplete_image` и `LogImage.test_unpack_image` (неполный образ — не 0) |
| `test_status_no_fs_and_no_file`, `test_dump_status_unchanged_for_read_data` | FatFs-статусов нет. Аналог — `wipe = 4` (флеш не прочиталась) и `hash_ok`; «журнала нет» решает разбор образа (`foreign`), тест `test_unpack_image` и `test_wipe_refused_without_journal` |
| `test_battery_fail_marked` | тот же |
| `test_raw_image_when_not_mounted`, `test_raw_read_failure_and_repeat` | `test_raw_image`, `test_raw_read_failure_and_repeat`; добавлен повтор через сброс `raw_done` |
| `test_layout_for_read_data` | новая раскладка: 14 слов, `raw_req` 0x24, `raw_done` 0x28 |
| `PatchCompiles.test_read_only`, `test_wipe` | те же. Компиляция теперь `-S`, а не `-fsyntax-only`: без кодогенерации gcc молчал о неиспользуемой `dump_wipe`, а сборка в CubeIDE предупредила бы |
| `PatchEol.test_crlf`, `test_lf` | те же (якорь сменился) |
| `ReadDataBench.test_wipe_survives_crlf`, `test_wiped_archive_kept`, `test_foreign_edit_not_flashed`, `test_battery_fail_not_empty_bank` | те же сценарии на `--swd`/`--wipe` с образом журнала |

Новые тесты:
- `Dumper`: `test_wipe_unlock_failed`, `_erase_failed_counted`, `_locked_chip_caught`,
  `test_feeds_watchdog`, `test_placed_before_journal_open`;
- `LogFormat.*`;
- `LogImage.*`;
- `UnpackUsb.*`;
- `ReadDataBench`: `test_usb_*` (в том числе поиск тома по корням), `test_swd_read`,
  `test_swd_chunk_retried`, `test_wipe_refused_when_board_changed`,
  `test_wipe_refused_without_journal`, `test_wipe_refused_on_pending_import`.

Итог: 75 тестов, 0 провалов (~24 с). Без gcc — 29 пропущено, остальное OK.

## Мутации

Скрипт меняет одну строку, гоняет `test_tools.py` (`-B`, кеш байткода чистится перед каждым
прогоном) и возвращает файл.

Первый прогон дал ложные результаты: мутация того же размера в ту же секунду оставила устаревший
`__pycache__`. Поэтому чистка кеша обязательна.

| № | Мутация | Что упало |
|---|---|---|
| M1 | хвост без проверки смежности номеров | `LogImage.test_restart_not_adjacent` |
| M2 | чтение без сверки seq | `LogImage.test_damaged` |
| M3 | `-nan` печатается как `nan` | все сценарии `LogImage` с замерами |
| M4 | `soil_valid` без `!bat_low` | то же |
| M5 | `bat_raw` со смещения 31 | `LogFormat.test_meas_layout`, `LogImage.*` |
| M6 | строка с `\n` вместо `\r\n` | `LogImage.*` |
| M7 | без моста через испорченный сектор | `LogImage.test_damaged` |
| M8 | голова — наименьший seq | `LogImage.*` |
| M9 | `ds_err` шириной 3 бита | `LogFormat.test_meas_bits` |
| M11 | `dump_wipe` без сверки хеша | `Dumper.test_wipe_refused_on_hash_mismatch` |
| M12 | стирание без проверки чтением | `Dumper.test_wipe_locked_chip_caught` |
| M13 | стирание без `hash_ok` | `Dumper.test_wipe_refused_on_read_error` |
| M14 | отказ чтения при хеше не прерывает подсчёт | `Dumper.test_wipe_refused_on_read_error` |
| M17 | повтор куска без сброса `raw_done` | `ReadDataBench.test_swd_chunk_retried` |
| M18 | `--image` без проверки размера | `LogImage.test_unpack_image` |
| M19 | `--usb` без сверки числа строк | `UnpackUsb.test_middle_line_lost` |
| M20 | незаконченный импорт не мешает `--image` | `LogImage.test_unpack_image` |
| M21 | `read_data.sh` без сверки копии с `info.txt` | `ReadDataBench.test_usb_copy`, `test_usb_short_copy_rejected` |
| M22 | проверочное чтение после стирания не делается | `Dumper.test_wipe_when_matches`, `_erase_failed_counted` |
| — | дампер без упоминания `dump_wipe` в проходе чтения | `PatchCompiles.test_read_only` (предупреждение `-Wunused-function`) |
| M23 | `--wipe` без условия `[ -n "$FAIL" ]` (мутация инспектора) | `ReadDataBench.test_wipe_refused_on_pending_import` (до `c7130d1` выживала) |
| M24 | поиск тома маской `/[d-z]` | `ReadDataBench.test_usb_default_roots_are_drive_letters` |

Выжили:
- **M10** — при «чужом» слоте 0 второй чужой слот не обрывает чтение сектора. Выход тот же, разница
  только в том, какой слот даёт номер сектора. Python повторяет правило прошивки ради верности,
  а не ради результата.
- **M16** — сорванный кусок не останавливает цикл. Его всё равно ловит следующий рубеж: размер
  образа или хеш всей флеш в `--image`. Код выхода тот же.
- Сначала выжили M1, M2 и M19. Под них добавлены сценарий `restart`, «годная запись не на своём
  месте» в `damaged` и тест `test_middle_line_lost`.

## Инспектор

Один субагент, только чтение, на копиях репозитория. Получил `_common.md`, обзор П5, бриф П5в,
требования владельца, `git diff 48897d5..fix/p5c-tools` на `982f005` и критерии — без переписки.

Подтвердил:
- Python-разбор построчно совпадает с `LogRing_Mount`/`LogRing_Read`/`VFat_Scan`;
- номера слов `dump_info_t` совпадают с чтением в `read_data.sh` (14 слов, 0x24/0x28);
- сторож пинается во всех долгих местах;
- вставка встаёт перед `Archive_Open` и откатывается байт в байт (LF и CRLF);
- правки только в `tools/` (без `syncheck.sh`) и SKILL.md;
- все проверки зелёные (без gcc — 29 пропущено).

Перепроверил 12 мутаций, в том числе две в копии `log_record.h` (смещение `sens_off`, формат
`vdd`): обе ловятся. Выжили две — см. ВАЖНО и мелочь 7.

**Вердикт: не готово** (БЛОК — 1, ВАЖНО — 1).

| Замечание | Уровень | Что сделано (`c7130d1`) |
|---|---|---|
| Поиск тома маской `/[d-z]`: в Git Bash не раскрывается (R7) — путь по умолчанию на Windows не нашёл бы диск, код 2 | БЛОК | Буквы перебираются явно, как в `cubeide.sh`; `FS_ROOTS` для стенда. Тесты: поиск без `FS_VOLUME` среди корней с чужим `info.txt`; текст скрипта без маски (M24) |
| Условие `[ -n "$FAIL" ]` перед `--wipe` не держалось тестом: при незаконченном импорте мутация стирала флеш | ВАЖНО | Тест стенда `test_wipe_refused_on_pending_import`: код 1, второй дампер не прошит, флеш цела (M23 убита) |
| Не сверялся `raw_off` куска | мелочь | Сверяется с `(n−1)·32768`, иначе повтор |
| Потерянный ответ второго дампера — выход по `set -e` с кодом 1 | мелочь | «итог стирания неизвестен», путь к копии образа, код 3 |
| Сводка на архиве из одного заголовка (после `--wipe`) роняет снятие в код 1 | мелочь | Сводка — справка: её код не влияет |
| Прошлые `_info.txt` (SWD) и `.img` (USB) не уходят в `.prev` | мелочь | Уходят |
| Образ любого размера, кратного 4 КБ, разбирается молча | мелочь | Предупреждение, если размер не 2 МБ (`log_image.py`, сводки) |
| `info.txt` из одной строки — IndexError в `--usb` | мелочь | Строка причины ищется по тексту |
| Пропуск строки ≥ 447 Б не покрыт тестом | мелочь | Оставлено: в прошивке недостижимо (худшая строка ~375 Б), константа сверяется с `VFAT_LINE_BUF` |
| ResourceWarning в тестах | мелочь | Файлы закрываются |
| Нет отчёта | — | этот файл |

После правок: 75 тестов, 0 провалов; syncheck OK; дампер встаёт и откатывается. Открытых БЛОК и
ВАЖНО нет.

## Проверки

- `bash -n tools/*.sh` — OK; `python3 -m py_compile tools/*.py` — OK.
- `python3 -I tools/test_tools.py` — 75 тестов, OK.
- `sh tools/syncheck.sh` → `syncheck: OK`.
- `python3 tools/dump_patch.py` встаёт в текущий `main.c`. Компиляция с заголовками HAL (флаги
  syncheck) — без предупреждений, в том числе с `--wipe`. `--revert` возвращает `main.c` байт в байт.
- Хостовые тесты прошивки не трогал и не перезапускал: прошивку параллельно правит другая сессия.

## Нужны правки вне моего владения

- **`Core/Src/main.c` — якорь дампера.** Дампер ищет строку `fs_ok = Archive_Open(boot_power_ok);`
  и `static uint32_t write_fails = 0;`. Сессия прошивки, которая их переименует или передвинет,
  сломает `read_data.sh --swd`. Сразу это поймают `PatchCompiles`/`Dumper.test_placed_before_journal_open`.
  Просьба к сессии прошивки — после правок `main.c` гонять `python3 -I tools/test_tools.py`.
- Других нет. `tools/hooks/pre-commit` ищет дампер и в `FATFS/*` — безвредно, оставлено.

## Проверить на плате (шаги для владельца, Windows / Git Bash)

1. **Копия с тома.**
   1. Воткнуть USB платы.
   2. В Git Bash из корня проекта: `./tools/read_data.sh`.
   3. Ждать «том: /e» (буква — ваша; поиск по буквам дисков — главное, что в облаке не
      проверить). Затем: «сверено с info.txt: N строк, последняя …», сводка, код 0 (`echo $?`).
   4. `build/data.txt` совпадает с файлом, скопированным Проводником: `cmp build/data.txt /e/data.txt`.
   5. Если диск не находится — `FS_VOLUME=/e ./tools/read_data.sh`.
2. **Снятие по SWD.** ST-Link подключён, USB платы — по желанию (с ним окно ловится сразу).
   1. Запустить `./tools/read_data.sh --swd`. Ожидается:
      - дампер прошит;
      - «снято 8/16/…/64 из 64 кусков» — 4–6 минут;
      - `образ: build/data.img (2097152 Б)`, сводка журнала, «записано»;
      - рабочая прошивка возвращена, код 0.
   2. `cmp build/data.txt <data.txt с тома из шага 1>` — совпадают, если между снятиями не было замеров.
   3. `git status` — `main.c` не изменён.
3. **Сторож во время снятия.** После `--swd` в следующих строках `data.txt` не растёт `boot_count`,
   а `reset_cause` — не сторож.
4. **Стирание (только на стенде).**
   1. `./tools/read_data.sh --wipe`. Ожидается:
      - «копия образа: build/archive/flash-…img»;
      - «флеш стёрта и проверена»;
      - код 0.
   2. Через час подключить USB: `data.txt` с одной-двумя строками, `info.txt` — первая метка свежая.
   3. Файлы в `build/archive` только для чтения.
5. **Сводки.**
   - `python tools/summarize_data.py build/data.img` и `python tools/analyze_run.py build/data.img` —
     первая строка «образ журнала …», дальше то же, что по `build/data.txt`.
   - `python tools/test_tools.py` под Windows: тесты с gcc и стенд bash пропускаются, остальные OK.

## Остаточные риски

- **Формулировки `STM32_Programmer_CLI`.** Для `-w32` вывод не разбирается: готовность определяется
  по `raw_done`. Для `-r` ожидается «read successfully», как и раньше. Синтаксис
  `-w32 <адрес> <значение>` — тот же, что в прежней шапке `dump_patch.py`; скриптом на плате он не
  гонялся — шаг 2.
- **Буква диска на Windows.** Перебор `/d`…`/z` проверен только стендом с подставными корнями; на
  Windows — шаг 1. Обходной путь — `FS_VOLUME=/e`.
- **Время снятия по SWD** — минуты: каждый вызов программатора заново подключается. Ускорить можно
  буфером больше 32 КБ (ОЗУ хватает: ~18 КБ занято из 96) или скриптом программатора — не делал,
  путь диагностический.
- **Хеш всей флеш перед стиранием** защищает от изменения флеш между проходами. Он не защищает от
  того, что образ разобран, а владелец его не открывал: копии в `build/archive` несменяемые.
- **Том ищется по `info.txt`.** Флешка с файлом `info.txt`, начинающимся с `FieldSensor `, будет
  принята за прибор. Сверка `data.txt` с её же `info.txt` пройдёт, если они согласованы.
- **Разбор — порт C-логики на Python.** Он сверен с прошивкой на 10 сценариях. При правке
  `LogRing_Mount`/`VFat_Scan` в прошивке тест поймает только то, что покрывают сценарии оракула;
  таблица полей сверяется всегда.
