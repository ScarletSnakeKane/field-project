#!/usr/bin/env bash
# Снять архив data.txt с платы через ST-Link, не подключая её по USB.
#
# Наружу плата отдаёт данные только как USB-накопитель, то есть нужен воткнутый
# в компьютер кабель. Когда его нет и есть только ST-Link, архив забирается так:
# сохранить main.c -> вставить дампер -> собрать -> прошить -> вычитать ОЗУ ->
# вернуть main.c -> собрать -> прошить обратно рабочую версию.
# Архив на флеш при этом только читается.
#
#   ./tools/read_data.sh [выходной_файл] [--wipe]
#
# --wipe очищает архив на плате, но только вторым, отдельным проходом: сначала
# файл снимается, сохраняется на диск и сверяется с платой (снят целиком, не
# окнами, хеш совпал), и лишь затем прошивается второй дампер, который удаляет
# файл при совпадении размера и хеша с сохранённым. Архив больше 32 КБ снимается
# окнами, на диск попадает только часть строк — такой --wipe не стирает.
#
# Прошлый выходной файл перед снятием переименовывается в <файл>.prev: иначе при
# неудачном снятии сводка показала бы старые данные как свежие (D3).
# Код выхода: 0 — снято (и стёрто, если просили); 1 — данных нет или снятие
# сломалось; 2 — нет программатора/ST-Link; 3 — снято, но стирание не сделано.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WIPE=""
ARGS=()
for a in "$@"; do
    if [ "$a" = "--wipe" ]; then WIPE=1; else ARGS+=("$a"); fi
done
OUT="${ARGS[0]:-$ROOT/build/data.txt}"
mkdir -p "$(dirname "$OUT")" "$ROOT/build"

. "$ROOT/tools/cubeide.sh"

SRC="$ROOT/Core/Src/main.c"
ELF="$ROOT/Debug/FieldSensor.elf"
ELF_W="$(cygpath -w "$ELF")"
TMPBIN="$(mktemp -u).bin"
FLASHED_DUMPER=0
RESTORED_FW=0
PATCHED_SUM=""

src_sum() { git hash-object --no-filters "$SRC"; }

echo ">> 1/6 сохраняю main.c"
# Копия снимается до установки trap: прерывание раньше этой строки оставило бы
# restore_src с пустой копией, и он записал бы пустоту поверх main.c (D4).
BACKUP="$(mktemp)"
cp "$SRC" "$BACKUP"
ORIG_SUM="$(src_sum)"

# Вернуть main.c. Копией перезаписываем только если файл ровно тот, что
# оставил патч. Если за время ожидания окна (до часа) main.c сохранили из
# CubeIDE, перезапись молча стёрла бы эти правки: тогда убираем из файла
# только вставку дампера, а копию «до снятия» кладём рядом (D5).
restore_src() {
    [ -n "$PATCHED_SUM" ] && [ -s "$BACKUP" ] || return 0
    local cur
    cur="$(src_sum)"
    if [ "$cur" = "$PATCHED_SUM" ]; then
        cp "$BACKUP" "$SRC"
        if [ "$(src_sum)" = "$ORIG_SUM" ]; then
            echo ">> main.c восстановлен (сверено по хешу)"
        else
            echo "!! main.c восстановлен, но хеш не совпал с исходным — проверьте git diff" >&2
        fi
    elif [ "$cur" != "$ORIG_SUM" ]; then
        cp "$BACKUP" "$ROOT/build/main.c.before-dump"
        if python "$ROOT/tools/dump_patch.py" --revert >/dev/null; then
            echo "!! main.c менялся во время снятия: ваши правки оставлены, вставка дампера убрана." >&2
        else
            echo "!! main.c менялся во время снятия, и вставку дампера убрать не удалось — уберите руками" >&2
        fi
        echo "   main.c до снятия: build/main.c.before-dump" >&2
    fi
    PATCHED_SUM=""
}

patch_src() {
    python "$ROOT/tools/dump_patch.py" "$@"
    PATCHED_SUM="$(src_sum)"
}

# Ловить окно бодрствования надо САМОЙ командой прошивки, а не подключением.
# Голое подключение ядро не удерживает: программатор при выходе отпускает его,
# плата тут же засыпает, и пойманный доступ пропадает. Команда записи захватывает
# ядро и держит его на всё время операции.
FLASH_WAIT_SEC="${FLASH_WAIT_SEC:-4200}"
flash_now() {
    [ -f "$ELF" ] || { echo "!! нет $ELF — сборка не прошла?" >&2; return 2; }
    prog_catch "$FLASH_WAIT_SEC" "verified successfully" \
        -c port=SWD mode=UR -w "$ELF_W" -v -rst
}

# На плате в этот момент может лежать дампер — прошивка, которая намеренно висит
# в while(1) и ничего не пишет. Оставить её там после аварийного выхода значит
# молча остановить сбор данных, причём со стороны плата выглядит рабочей.
# Поэтому рабочая версия возвращается при любом исходе, в том числе по Ctrl+C.
on_exit() {
    local code=$?
    trap - EXIT
    # Откат тоже может ждать окно до часа. Повторный Ctrl+C его обрывает — но не
    # молча: на плате остаётся дампер, и человек должен это знать.
    trap 'echo "!! откат прерван. На плате может остаться дампер (висит в while(1), не пишет, садит банку): ./tools/dev.sh flash" >&2; exit 130' INT TERM
    restore_src
    if [ "$FLASHED_DUMPER" = "1" ] && [ "$RESTORED_FW" != "1" ]; then
        echo ">> аварийный откат: возвращаю рабочую прошивку" >&2
        if "$ROOT/tools/dev.sh" build >/dev/null 2>&1 && flash_now; then
            echo ">> рабочая прошивка возвращена" >&2
        else
            echo "!! НЕ УДАЛОСЬ. На плате дампер. Прошейте вручную: ./tools/dev.sh flash" >&2
        fi
    fi
    rm -f "$BACKUP" "$TMPBIN" "$TMPBIN.cut"
    exit "$code"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Плата почти всё время лежит в STOP, где SWD обесточен, поэтому попасть в неё
# можно только в окно бодрствования — отсюда повторы.
retry() {
    local n=$1 need=$2; shift 2
    local i out
    for ((i = 1; i <= n; i++)); do
        # Программатор разделяет строки одним CR, без LF. Удалять CR нельзя:
        # весь вывод склеится в одну строку и построчный разбор рассыплется —
        # поэтому здесь замена CR на перевод строки, а не удаление.
        out=$("$PROG" "$@" 2>&1 | tr '\r' '\n' || true)
        if echo "$out" | grep -q "$need"; then
            echo "$out"
            return 0
        fi
        # Пауза обязательна. Без неё попытки идут вплотную, период их повторения
        # оказывается сравним с периодом сна платы, и они раз за разом попадают
        # в одну и ту же фазу — в сон. С паузой фаза уползает, и окно ловится.
        sleep 5
    done
    echo "$out" >&2
    echo "не удалось за $n попыток: $need" >&2
    return 1
}

# Адреса берём из карты линковки, а не прибиваем константами: они уезжают от
# любой правки, а молча прочитанный не тот кусок ОЗУ выглядел бы как испорченные
# данные, а не как ошибка.
locate_map() {
    BUF=$(grep -m1 '\.bss\.dump_buf' Debug/FieldSensor.map | awk '{print $2}')
    INF=$(grep -m1 '\.bss\.dump_info' Debug/FieldSensor.map | awk '{print $2}')
    if [ -z "$INF" ]; then
        INF=$(grep -A1 -m1 '\.bss\.dump_info' Debug/FieldSensor.map | tail -1 | awk '{print $1}')
    fi
    if [ -z "$BUF" ]; then
        BUF=$(grep -A1 -m1 '\.bss\.dump_buf' Debug/FieldSensor.map | tail -1 | awk '{print $1}')
    fi
}

# Ответ приходит строками вида "0x20009398 : D00DFEED 000001B9 000001B9 00000A73".
# Тринадцать слов структуры ложатся на несколько строк, поэтому склеиваем все
# слова подряд, а не разбираем одну конкретную строку. Раскладка — dump_info_t
# в tools/dump_patch.py.
read_info() {
    "$PROG" -c port=SWD mode=HOTPLUG -r32 "$INF" 52 2>&1 | tr '\r' '\n' \
        | grep -E "^0x[0-9A-Fa-f]+ : " | sed 's/^[^:]*: //' | tr ' ' '\n' \
        | grep -E '^[0-9A-F]{8}$' || true
}

# Ждём именно маркер, а не просто успешный ответ отладчика. Читать структуру
# один раз нельзя: дампер к моменту первого запроса ещё работает — монтирует
# том, вычитывает архив (чем он длиннее, тем дольше) и выдерживает паузу на
# выход датчиков в режим. Прочитанная в этот момент структура заполнена лишь
# частично, и раньше это выглядело как «дамп не выполнился», хотя он просто
# не успел.
wait_dump() {
    local attempt
    magic=""
    for attempt in $(seq 1 20); do
        set -- $(read_info)
        if [ "${1:-}" = "D00DFEED" ] && [ $# -ge 13 ]; then
            magic=$1; fsize=$((16#$2)); len=$((16#$3))
            win_bytes=$((16#$4)); win_count=$((16#$5)); win_step=$((16#$6))
            soil_mv=$7; bat_mv=$((16#$8)); vdd_mv=$((16#$9)); bat_pct=$((16#${10}))
            status=$((16#${11})); hash="0x${12}"; wipe=$((16#${13}))
            return 0
        fi
        sleep 2
    done
    echo "дамп не выполнился за 20 попыток" >&2
    return 1
}

# Почему снято не всё: «том не смонтирован» и «файла нет» раньше выглядели
# одинаково — «файл на плате пуст» (D3).
status_text() {
    local st=$(( status & 0xFF )) fr=$(( (status >> 8) & 0xFF ))
    case $st in
        0) echo "" ;;
        1) echo "файловая система не смонтирована (fs_ok=0) — запись на плате стоит" ;;
        2) if [ "$fr" -eq 4 ]; then echo "файла data.txt на томе нет";
           else echo "f_open вернул FRESULT $fr"; fi ;;
        3) echo "f_read вернул FRESULT $fr — снято $len из $fsize Б" ;;
        4) echo "f_lseek вернул FRESULT $fr — снято $len из $fsize Б" ;;
        *) echo "неизвестный статус дампера 0x$(printf %X "$status")" ;;
    esac
}

require_prog
require_stlink

# Старый файл убираем до снятия, а не после: любой сбой ниже иначе оставил бы
# на месте выходного файла прошлый архив, и сводка выдала бы его за свежий.
if [ -e "$OUT" ]; then
    mv -f "$OUT" "$OUT.prev"
    echo "   прошлый снимок отложен: $OUT.prev"
fi

FAIL=""       # почему данных нет — код 1
WIPE_FAIL=""  # почему не стёрто — код 3

echo ">> 2/6 вставляю дампер и собираю"
patch_src
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 3/6 прошиваю дампер"
# Отметка ставится ДО прошивки: прерванная на середине запись тоже оставляет
# на плате не рабочую версию, а лишний откат безвреден (D4).
FLASHED_DUMPER=1
flash_now

echo ">> 4/6 читаю ОЗУ"
locate_map
wait_dump
echo "   файл на флеш: $fsize Б, снято: $len Б"
if [ "$win_bytes" -gt 0 ]; then
    echo "   архив больше буфера: взято $win_count окон по $win_bytes Б с шагом $win_step Б"
fi
why="$(status_text)"
[ -z "$why" ] || echo "   !! $why" >&2

echo "   живой замер: батарея ${bat_mv} мВ (${bat_pct}%), VDD ${vdd_mv} мВ"
if [ "$soil_mv" = "FFFFFFFF" ]; then
    echo "   датчик почвы: АЦП не ответил"
else
    echo "   датчик почвы: $((16#$soil_mv)) мВ до пересчёта"
fi

if [ "$len" -gt 0 ]; then
    # Программатор читает только кратно четырём байтам, поэтому лишний хвост
    # отрезаем уже на стороне компьютера.
    rounded=$(( (len + 3) / 4 * 4 ))
    retry 20 "read successfully" \
        -c port=SWD mode=HOTPLUG -r "$BUF" "$rounded" "$(cygpath -w "$TMPBIN")" >/dev/null
    python -c "import sys; open(sys.argv[1],'wb').write(open(sys.argv[2],'rb').read()[:int(sys.argv[3])])" \
        "$TMPBIN.cut" "$TMPBIN" "$len"
    # Хеш сверяется до записи: прочитанный не тот кусок ОЗУ не попадёт на диск.
    if python "$ROOT/tools/unpack_dump.py" "$TMPBIN.cut" "$OUT" "$win_bytes" "$win_count" "$hash"; then
        echo "   записано: $OUT"
        [ -z "$why" ] || FAIL="снято не всё: $why"
    else
        FAIL="прочитанное по SWD не совпало с буфером дампера"
    fi
    rm -f "$TMPBIN" "$TMPBIN.cut"
else
    FAIL="${why:-файл на плате пуст (0 Б)}"
fi

if [ -n "$WIPE" ]; then
    if [ -n "$FAIL" ]; then
        WIPE_FAIL="снятие не удалось — стирать нечем подтверждённое"
    elif [ "$win_bytes" -gt 0 ]; then
        WIPE_FAIL="архив $fsize Б больше буфера 32 КБ и снят окнами — на диске только часть строк. Снимите файл целиком через USB самой платы (том появится как флешка) и удалите его там"
    elif [ "$len" -ne "$fsize" ]; then
        WIPE_FAIL="снято $len Б из $fsize"
    elif [ "$(python "$ROOT/tools/unpack_dump.py" --fnv "$OUT")" != "$hash" ]; then
        WIPE_FAIL="файл на диске не совпал с прочитанным с платы"
    else
        echo ">> 4б/6 стираю архив: второй дампер удалит файл, только если он тот же ($fsize Б, хеш $hash)"
        saved_size=$fsize
        restore_src
        patch_src --wipe "$fsize" "$hash"
        "$ROOT/tools/dev.sh" build >/dev/null
        # Плата сейчас крутится в первом дампере и не спит — окно ловить не надо.
        flash_now
        locate_map
        wait_dump
        case $(( wipe & 0xFF )) in
            1) echo "   архив на плате удалён; рабочая прошивка создаст его заново" ;;
            2) WIPE_FAIL="дампер отказался: файл на плате уже не тот, что сохранён (на диске $saved_size Б, на плате $fsize Б)" ;;
            3) WIPE_FAIL="f_unlink вернул FRESULT $(( (wipe >> 8) & 0xFF ))" ;;
            *) WIPE_FAIL="дампер не сообщил итог очистки (wipe=$wipe)" ;;
        esac
    fi
fi

echo ">> 5/6 возвращаю рабочую версию"
restore_src
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 6/6 прошиваю рабочую версию обратно"
flash_now
RESTORED_FW=1
echo ">> готово"

if [ -n "$FAIL" ]; then
    echo >&2
    echo "!! $FAIL" >&2
    if [ ! -e "$OUT" ]; then
        echo "!! свежих данных нет, сводку не строю (прошлый снимок, если был: $OUT.prev)" >&2
        exit 1
    fi
fi

echo
if [ "${win_bytes:-0}" -gt 0 ]; then
    python "$ROOT/tools/summarize_data.py" "$OUT" --windowed
else
    python "$ROOT/tools/summarize_data.py" "$OUT"
fi

if [ -n "$FAIL" ]; then
    exit 1
fi
if [ -n "$WIPE_FAIL" ]; then
    echo "!! архив НЕ стёрт: $WIPE_FAIL" >&2
    exit 3
fi
