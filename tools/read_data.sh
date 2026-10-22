#!/usr/bin/env bash
# Снять архив с платы.
#
# Архив на флеш — кольцевой журнал (Core/Inc/log_ring.h). По USB плата
# отдаёт его как диск FIELDSENSOR только для чтения: data.txt (CSV целиком,
# от старой строки к новой), info.txt и, пока жив текст старого архива,
# data_old.txt. Это самый надёжный путь, и он идёт по умолчанию:
#
#   ./tools/read_data.sh [выходной_файл]          копировать с тома USB
#   ./tools/read_data.sh [выходной_файл] --swd    сырой образ флеш по ST-Link
#   ./tools/read_data.sh [выходной_файл] --wipe   --swd и затем стереть флеш
#
# USB: воткнуть USB самой платы; диск появляется через 5–20 с (прошивка
# собирает том по журналу). Скрипт ждёт его до USB_WAIT_SEC (60 с), копирует
# файлы и сверяет data.txt с info.txt того же тома (число строк, последняя
# метка). Где искать том: FS_VOLUME=<каталог тома>; иначе — диски D:..Z:
# (Git Bash), /media, /run/media, /Volumes (FS_ROOTS=<каталоги> — свой список).
#
# --swd — когда USB нет или не поднимается, и для диагностики флеш: дампер
# (tools/dump_patch.py) вставляется в main.c перед открытием журнала,
# прошивается, отдаёт всю флеш кусками по 32 КБ, а после снятия прошивается
# обратно рабочая версия. Образ — <выходной_файл без .txt>.img; data.txt и
# data_old.txt из него собирает tools/log_image.py — те же байты, что на томе.
#
# --wipe стирает флеш целиком, только вторым, отдельным проходом и только
# если образ снят полностью, сошёлся с хешем всей флеш, журнал в нём
# разобран без ошибок и несменяемая копия образа и CSV легла в build/archive/
# (flash-<дата>-<хеш>.img, data-<дата>-<хеш>.txt — не перезаписывается и не
# удаляется никогда). Второй дампер заново считает хеш флеш и стирает её,
# только если он совпал с сохранённым. Заказчику стирание не нужно: кольцо
# само уносит старейшие записи, прибор пишет всегда.
#
# Прошлые выходные файлы перед снятием переименовываются в .prev: иначе при
# неудачном снятии сводка показала бы старые данные как свежие (D3).
# Рабочая прошивка после --swd собирается из main.c, каким он был ДО снятия,
# байт-в-байт. Если main.c за время снятия правили, правки откладываются в
# build/main.c.yours-<дата> и возвращаются в main.c только после того, как на
# плату легла рабочая версия: недописанный код в поле не уходит (R4).
#
# Код выхода: 0 — снято (и стёрто, если просили); 1 — данных нет или снятие
# сломалось; 2 — нет тома USB (без --swd) или нет программатора/ST-Link;
# 3 — снято, но стирание не сделано (причина в последней строке).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WIPE=""
SWD=""
ARGS=()
for a in "$@"; do
    case "$a" in
        --wipe) WIPE=1; SWD=1 ;;
        --swd) SWD=1 ;;
        *) ARGS+=("$a") ;;
    esac
done
OUT="${ARGS[0]:-$ROOT/build/data.txt}"
BASE="${OUT%.txt}"
OLD="${BASE}_old.txt"
INFO="${BASE}_info.txt"
IMG="${BASE}.img"
mkdir -p "$(dirname "$OUT")" "$ROOT/build"

stamp() { date +%Y%m%d-%H%M%S; }

# Старое убираем до снятия, а не после: любой сбой ниже иначе оставил бы на
# месте выходного файла прошлый архив, и сводка выдала бы его за свежий.
shelve() {
    local f
    for f in "$@"; do
        if [ -e "$f" ]; then
            mv -f "$f" "$f.prev"
            echo "   прошлый снимок отложен: $f.prev"
        fi
    done
}

# Сводка — справка, а не проверка: на пустом архиве (только заголовок, сразу
# после --wipe) она отвечает 1, и снятие из-за этого не должно считаться
# сорванным.
summary() {
    echo
    python "$ROOT/tools/summarize_data.py" "$OUT" || true
}

# ------------------------------------------------------------------ USB

# Том узнаётся по info.txt: его первая строка — «FieldSensor <сборка>».
is_volume() {
    [ -f "$1/info.txt" ] && head -c 12 "$1/info.txt" 2>/dev/null | grep -q '^FieldSensor '
}

find_volume() {
    local d
    if [ -n "${FS_VOLUME:-}" ]; then
        is_volume "$FS_VOLUME" && echo "$FS_VOLUME"
        return 0
    fi
    # Буквы дисков перебираются явно: в Git Bash /c, /d — точки монтирования,
    # корневой каталог их не перечисляет, и маска /[d-z] не раскрывается (R7,
    # как в cubeide.sh). /a и /b не трогаем: опрос пустого дисковода висит.
    local roots="${FS_ROOTS:-}" drv
    if [ -z "$roots" ]; then
        for drv in d e f g h i j k l m n o p q r s t u v w x y z; do roots="$roots /$drv"; done
        roots="$roots $(echo /media/*/* /run/media/*/* /Volumes/*)"
    fi
    for d in $roots; do
        [ -d "$d" ] || continue
        if is_volume "$d"; then
            echo "$d"
            return 0
        fi
    done
    return 0
}

read_usb() {
    local vol="" waited=0 wait="${USB_WAIT_SEC:-60}"
    while :; do
        vol="$(find_volume)"
        [ -n "$vol" ] && break
        if [ "$waited" -ge "$wait" ]; then
            echo "!! диска FIELDSENSOR не видно за $wait с." >&2
            echo "   Воткните USB самой платы (диск появляется через 5–20 с) или задайте" >&2
            echo "   FS_VOLUME=<путь к диску>. Без USB — по ST-Link: $0 --swd" >&2
            exit 2
        fi
        [ "$waited" -eq 0 ] && echo "   жду диск FIELDSENSOR (до $wait с): воткните USB самой платы"
        sleep 2
        waited=$((waited + 2))
    done
    echo ">> том: $vol"
    shelve "$OUT" "$OLD" "$INFO" "$IMG"
    cp "$vol/info.txt" "$INFO"
    if [ ! -f "$vol/data.txt" ]; then
        # Журнал не открыт: на томе только info.txt с причиной.
        sed 's/\r$//' "$INFO" >&2
        echo "!! на томе нет data.txt — архив на плате сейчас недоступен (причина выше)" >&2
        exit 1
    fi
    # Сначала во временный файл: сорванное копирование не должно лечь на
    # место архива.
    if ! cp "$vol/data.txt" "$OUT.part"; then
        rm -f "$OUT.part"
        echo "!! копирование data.txt сорвалось (кабель? плата уснула?) — повторите" >&2
        exit 1
    fi
    if ! python "$ROOT/tools/unpack_dump.py" --usb "$OUT.part" "$INFO"; then
        rm -f "$OUT.part"
        echo "!! скопированный data.txt не сошёлся с info.txt тома — повторите копирование" >&2
        exit 1
    fi
    mv -f "$OUT.part" "$OUT"
    echo "   записано: $OUT"
    if [ -f "$vol/data_old.txt" ]; then
        cp "$vol/data_old.txt" "$OLD"
        echo "   текст старого архива: $OLD"
    fi
    summary
    exit 0
}

[ -n "$SWD" ] || read_usb

# ------------------------------------------------------------------ SWD

. "$ROOT/tools/cubeide.sh"

SRC="$ROOT/Core/Src/main.c"
ELF="$ROOT/Debug/FieldSensor.elf"
ELF_W="$(cygpath -w "$ELF")"
TMPBIN="$(mktemp -u).bin"
FLASHED_DUMPER=0
RESTORED_FW=0
PATCHED_SUM=""
YOURS=""      # копия main.c с правками владельца, сделанными за время снятия
ARCHIVE=""    # несменяемая копия образа, снятого перед стиранием

# Всё, что сравнивается после $(...), чистится от CR: на Windows программы
# пишут CRLF, а Git Bash срезает только LF, и «0x1234\r» != «0x1234» (R1).
src_sum() { git hash-object --no-filters "$SRC" | tr -d '\r'; }

echo ">> 1/6 сохраняю main.c"
# Копия снимается до установки trap: прерывание раньше этой строки оставило бы
# restore_src с пустой копией, и он записал бы пустоту поверх main.c (D4).
BACKUP="$(mktemp)"
cp "$SRC" "$BACKUP"
ORIG_SUM="$(src_sum)"

# Вернуть main.c байт-в-байт из копии, снятой до патча (D5), со сверкой хеша.
# Если за время ожидания окна (до часа) main.c сохранили из CubeIDE, такой
# файл откладывается в build/main.c.yours-<дата> (без вставки дампера), а
# рабочая прошивка собирается из копии «до снятия» (R4).
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
    [ -z "$ARCHIVE" ] || echo ">> копия образа, снятая перед --wipe: ${ARCHIVE#$ROOT/} (не перезаписывается)"
    rm -f "$BACKUP" "$TMPBIN" "$TMPBIN.cut"
    exit "$code"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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

# Слово dump_info по смещению: адрес для -w32.
info_addr() { printf "0x%08X" $(( INF + $1 )); }

# Ответ приходит строками вида "0x20009398 : D00DFEED 00200000 ...". Слова
# структуры ложатся на несколько строк, поэтому склеиваем все слова подряд,
# а не разбираем одну конкретную строку. Раскладка — dump_info_t в
# tools/dump_patch.py (14 слов).
read_info() {
    "$PROG" -c port=SWD mode=HOTPLUG -r32 "$INF" 56 2>&1 | tr '\r' '\n' \
        | grep -E "^0x[0-9A-Fa-f]+ : " | sed 's/^[^:]*: //' | tr ' ' '\n' \
        | grep -E '^[0-9A-F]{8}$' || true
}

# Ждём именно маркер, а не просто успешный ответ отладчика: дампер к моменту
# первого запроса ещё считает хеш всей флеш (секунды), а во втором проходе —
# ещё и стирает её (до ~15 с). Структура, прочитанная раньше, заполнена лишь
# частично.
wait_dump() {
    local attempt
    for attempt in $(seq 1 "${DUMP_WAIT_TRIES:-60}"); do
        set -- $(read_info)
        if [ "${1:-}" = "D00DFEED" ] && [ $# -ge 14 ]; then
            raw_total=$((16#$2)); soil_mv=$3; bat_mv=$4; vdd_mv=$5; bat_pct=$6
            flash_hash="${7}"; hash_ok=$((16#$8)); wipe=$((16#$9))
            return 0
        fi
        sleep 2
    done
    echo "дамп не выполнился за ${DUMP_WAIT_TRIES:-60} попыток" >&2
    return 1
}

# Один кусок образа. Повтор того же номера дампер выполняет, только если
# raw_done сброшен, поэтому перед запросом пишется 0 в raw_done. Готовность —
# по raw_done == n (вывод -w32 не разбирается: формулировки у версий
# программатора разные).
get_chunk() {
    local n=$1 try i w len
    for try in 1 2 3; do
        "$PROG" -c port=SWD mode=HOTPLUG -w32 "$(info_addr 0x28)" 0 >/dev/null 2>&1 || true
        "$PROG" -c port=SWD mode=HOTPLUG -w32 "$(info_addr 0x24)" "$n" >/dev/null 2>&1 || true
        for i in $(seq 1 20); do
            w=($(read_info))
            if [ "${#w[@]}" -ge 14 ] && [ $((16#${w[10]})) -eq "$n" ]; then
                break
            fi
            w=()
            sleep 1
        done
        [ "${#w[@]}" -ge 14 ] || continue
        len=$((16#${w[12]}))
        [ "$len" -gt 0 ] || continue
        # Смещение — то, что просили: искажённое чтение слов не положит кусок
        # не на своё место (без хеша всей флеш его бы никто не заметил).
        [ $((16#${w[11]})) -eq $(( (n - 1) * 32768 )) ] || continue
        # Программатор читает только кратно четырём байтам, поэтому лишний хвост
        # отрезаем уже на стороне компьютера.
        "$PROG" -c port=SWD mode=HOTPLUG -r "$BUF" $(( (len + 3) / 4 * 4 )) "$(cygpath -w "$TMPBIN")" \
            2>&1 | tr '\r' '\n' | grep -q "read successfully" || continue
        python -c "import sys; open(sys.argv[1],'wb').write(open(sys.argv[2],'rb').read()[:int(sys.argv[3])])" \
            "$TMPBIN.cut" "$TMPBIN" "$len"
        # Хеш сверяется до записи: прочитанный не тот кусок ОЗУ в образ не попадёт.
        python "$ROOT/tools/unpack_dump.py" --raw "$TMPBIN.cut" "$IMG.part" "${w[11]}" "${w[13]}" \
            >/dev/null && return 0
    done
    return 1
}

# Несменяемая копия перед стиранием: после него на плате архива нет, а
# build/data.img следующие снятия затирают через .prev (R5). Имя уникально,
# существующий файл не трогается, копия сверяется по хешу.
save_archive() {
    local dir="$ROOT/build/archive" tag n=0
    mkdir -p "$dir" || return 1
    tag="$(stamp)-$flash_hash"
    while [ -e "$dir/flash-$tag.img" ]; do n=$((n + 1)); tag="$(stamp)-$flash_hash-$n"; done
    cp "$IMG" "$dir/flash-$tag.img" || return 1
    python "$ROOT/tools/unpack_dump.py" --check "$dir/flash-$tag.img" "0x$flash_hash" || return 1
    cp "$OUT" "$dir/data-$tag.txt" || return 1
    if [ -f "$OLD" ]; then
        cp "$OLD" "$dir/data_old-$tag.txt" || return 1
    fi
    chmod a-w "$dir/flash-$tag.img" "$dir/data-$tag.txt" "$dir/data_old-$tag.txt" 2>/dev/null || true
    ARCHIVE="$dir/flash-$tag.img"
    echo "   копия образа: ${ARCHIVE#$ROOT/}"
}

require_prog
require_stlink

shelve "$OUT" "$OLD" "$IMG" "$INFO"
rm -f "$IMG.part"

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

echo ">> 4/6 снимаю образ флеш"
locate_map
wait_dump
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
[ "$hash_ok" -eq 1 ] || echo "   !! флеш не прочиталась целиком при подсчёте хеша — образ сверить не с чем" >&2

nchunks=$(( (raw_total + 32767) / 32768 ))
for ((n = 1; n <= nchunks; n++)); do
    if ! get_chunk "$n"; then
        FAIL="кусок $n из $nchunks не снят за три попытки — образ неполный (${IMG#$ROOT/}.part)"
        break
    fi
    [ $((n % 8)) -ne 0 ] || echo "   снято $n из $nchunks кусков"
done
rm -f "$TMPBIN" "$TMPBIN.cut"

if [ -z "$FAIL" ]; then
    mv -f "$IMG.part" "$IMG"
    echo "   образ: $IMG ($raw_total Б)"
    hash_arg=()
    [ "$hash_ok" -eq 1 ] && hash_arg=("0x$flash_hash")
    if python "$ROOT/tools/unpack_dump.py" --image "$IMG" "$OUT" "$OLD" "$raw_total" ${hash_arg[@]+"${hash_arg[@]}"}; then
        echo "   записано: $OUT"
    elif [ -e "$OUT" ]; then
        FAIL="журнал разобран с оговорками (выше) — data.txt собран, но образ стоит разобрать вручную"
    else
        FAIL="из образа не собрать data.txt (причина выше); образ сохранён: $IMG"
    fi
fi

if [ -n "$WIPE" ]; then
    if [ -n "$FAIL" ]; then
        WIPE_FAIL="образ не снят полностью или не разобран — стирать нечем подтверждённое"
    elif [ "$hash_ok" -ne 1 ]; then
        WIPE_FAIL="флеш не прочиталась целиком при подсчёте хеша — образ не с чем сверить"
    elif ! python "$ROOT/tools/unpack_dump.py" --check "$IMG" "0x$flash_hash"; then
        # Сверка кодом возврата, а не сравнением строк: без CRLF-ловушки (R1).
        WIPE_FAIL="образ на диске не совпал с флеш"
    elif ! save_archive; then
        WIPE_FAIL="не удалось сохранить несменяемую копию в build/archive"
    else
        echo ">> 4б/6 стираю флеш: второй дампер сотрёт её, только если она та же ($raw_total Б, хеш 0x$flash_hash)"
        saved_hash=$flash_hash
        restore_src
        patch_src --wipe "$raw_total" "0x$flash_hash"
        "$ROOT/tools/dev.sh" build >/dev/null
        # Плата сейчас крутится в первом дампере и не спит — окно ловить не надо.
        flash_now
        locate_map
        if ! wait_dump; then
            wipe=-1
        fi
        case $(( wipe & 0xFF )) in
            1) echo "   флеш стёрта и проверена; рабочая прошивка начнёт журнал с нуля" ;;
            2) WIPE_FAIL="дампер отказался: флеш уже не та, что сохранена (на диске 0x$saved_hash, на плате 0x$flash_hash)" ;;
            3) WIPE_FAIL="стирание не удалось: защита не снялась или $(( wipe >> 8 )) секторов не стёрлись — архив, возможно, стёрт частично; копия: ${ARCHIVE#$ROOT/}" ;;
            4) WIPE_FAIL="флеш не прочиталась при сверке — не тронута" ;;
            *) WIPE_FAIL="дампер не сообщил итог стирания (wipe=$wipe) — стёрта ли флеш, неизвестно; копия образа: ${ARCHIVE#$ROOT/}" ;;
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

summary

if [ -n "$FAIL" ]; then
    exit 1
fi
if [ -n "$WIPE_FAIL" ]; then
    echo "!! флеш НЕ стёрта: $WIPE_FAIL" >&2
    exit 3
fi
