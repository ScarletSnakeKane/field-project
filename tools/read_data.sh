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
# Перед стиранием снятый файл копируется в build/archive/data-<дата>-<хеш>.txt:
# build/data.txt следующие снятия сдвигают в .prev и затирают, а копия в
# build/archive не перезаписывается и не удаляется никогда (R5).
#
# Прошлый выходной файл перед снятием переименовывается в <файл>.prev: иначе при
# неудачном снятии сводка показала бы старые данные как свежие (D3).
# Рабочая прошивка после снятия собирается из main.c, каким он был ДО снятия,
# байт-в-байт. Если main.c за время снятия правили, правки откладываются в
# build/main.c.yours-<дата> и возвращаются в main.c только после того, как на
# плату легла рабочая версия: недописанный код в поле не уходит (R4).
#
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
YOURS=""      # копия main.c с правками владельца, сделанными за время снятия
ARCHIVE=""    # несменяемая копия архива, снятого перед стиранием

# Всё, что сравнивается после $(...), чистится от CR: на Windows программы
# пишут CRLF, а Git Bash срезает только LF, и «0x1234\r» != «0x1234» (R1).
src_sum() { git hash-object --no-filters "$SRC" | tr -d '\r'; }
stamp() { date +%Y%m%d-%H%M%S; }

echo ">> 1/6 сохраняю main.c"
# Копия снимается до установки trap: прерывание раньше этой строки оставило бы
# restore_src с пустой копией, и он записал бы пустоту поверх main.c (D4).
BACKUP="$(mktemp)"
cp "$SRC" "$BACKUP"
ORIG_SUM="$(src_sum)"

# Вернуть main.c байт-в-байт из копии, снятой до патча (D5), со сверкой хеша.
# Если за время ожидания окна (до часа) main.c сохранили из CubeIDE, раньше из
# файла убиралась только вставка дампера, и на плату уходила недописанная
# правка, а если она не собиралась — на плате оставался дампер (R4). Теперь
# такой файл откладывается в build/main.c.yours-<дата> (без вставки дампера),
# а рабочая прошивка собирается из копии «до снятия».
restore_src() {
    [ -n "$PATCHED_SUM" ] && [ -s "$BACKUP" ] || return 0
    local cur yours
    cur="$(src_sum)"
    if [ "$cur" != "$PATCHED_SUM" ] && [ "$cur" != "$ORIG_SUM" ]; then
        yours="$ROOT/build/main.c.yours-$(stamp)"
        while [ -e "$yours" ]; do yours="$yours.1"; done
        cp "$SRC" "$yours"
        echo "!! ======================================================================" >&2
        echo "!! main.c ИЗМЕНИЛИ во время снятия. На плату он НЕ пойдёт: рабочая" >&2
        echo "!! прошивка соберётся из main.c, каким он был до снятия." >&2
        if python "$ROOT/tools/dump_patch.py" --revert "$yours" >/dev/null; then
            echo "!! Ваши правки (без вставки дампера): ${yours#$ROOT/}" >&2
            YOURS="$yours"
        else
            echo "!! Ваши правки отложены в ${yours#$ROOT/}, но вставку дампера из" >&2
            echo "!! копии убрать не удалось — уберите её руками, в main.c сами не вернутся." >&2
            YOURS=""
        fi
        echo "!! ======================================================================" >&2
    fi
    if [ "$cur" != "$ORIG_SUM" ]; then
        cp "$BACKUP" "$SRC"
    fi
    if [ "$(src_sum)" = "$ORIG_SUM" ]; then
        echo ">> main.c восстановлен из копии до снятия (сверено по хешу)"
    else
        echo "!! main.c восстановлен, но хеш не совпал с исходным — проверьте git diff" >&2
    fi
    PATCHED_SUM=""
}

# Вернуть в main.c правки владельца — только когда на плате уже рабочая
# версия (или дампер и не прошивался). Иначе подсказка «./tools/dev.sh flash»
# прошила бы в поле ту самую недописанную правку.
give_back_yours() {
    [ -n "$YOURS" ] || return 0
    if [ "$FLASHED_DUMPER" = "1" ] && [ "$RESTORED_FW" != "1" ]; then
        echo "!! ваши правки main.c НЕ возвращены (на плате не рабочая версия): ${YOURS#$ROOT/}" >&2
    elif [ "$(src_sum)" != "$ORIG_SUM" ]; then
        echo "!! main.c снова менялся — ваши отложенные правки остались в ${YOURS#$ROOT/}" >&2
    else
        cp "$YOURS" "$SRC"
        echo "!! ваши правки возвращены в main.c (копия: ${YOURS#$ROOT/}); на плате — версия ДО них" >&2
    fi
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
            RESTORED_FW=1
            echo ">> рабочая прошивка возвращена" >&2
        else
            echo "!! НЕ УДАЛОСЬ. На плате дампер. Прошейте вручную: ./tools/dev.sh flash" >&2
        fi
    fi
    give_back_yours
    [ -z "$ARCHIVE" ] || echo ">> копия архива, снятая перед --wipe: ${ARCHIVE#$ROOT/} (не перезаписывается)"
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
            soil_mv=$7; bat_mv=$8; vdd_mv=$9; bat_pct=${10}
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

# Несменяемая копия перед стиранием: после него на плате архива нет, а
# build/data.txt два следующих снятия затирают через .prev (R5). Имя
# уникально, существующий файл не трогается, копия сверяется по хешу.
save_archive() {
    local dir="$ROOT/build/archive" base f n=0
    mkdir -p "$dir" || return 1
    base="$dir/data-$(stamp)-${hash#0x}"
    f="$base.txt"
    while [ -e "$f" ]; do n=$((n + 1)); f="$base-$n.txt"; done
    cp "$OUT" "$f" || return 1
    python "$ROOT/tools/unpack_dump.py" --check "$f" "$hash" || return 1
    chmod a-w "$f" 2>/dev/null || true
    ARCHIVE="$f"
    echo "   копия архива: ${f#$ROOT/}"
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

if [ "$bat_mv" = "FFFFFFFF" ]; then
    echo "   живой замер: батарея — замер не удался (нет данных о банке и VDD)"
else
    echo "   живой замер: батарея $((16#$bat_mv)) мВ ($((16#$bat_pct))%), VDD $((16#$vdd_mv)) мВ"
fi
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
    elif ! python "$ROOT/tools/unpack_dump.py" --check "$OUT" "$hash"; then
        # Сверка кодом возврата, а не сравнением строк: без CRLF-ловушки (R1).
        WIPE_FAIL="файл на диске не совпал с прочитанным с платы"
    elif ! save_archive; then
        WIPE_FAIL="не удалось сохранить несменяемую копию в build/archive"
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
