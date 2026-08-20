#!/usr/bin/env bash
# Снять архив data.txt с платы через ST-Link, не подключая её по USB.
#
# Наружу плата отдаёт данные только как USB-накопитель, то есть нужен воткнутый
# в компьютер кабель. Когда его нет и есть только ST-Link, архив забирается так:
# сохранить main.c -> вставить дампер -> собрать -> прошить -> вычитать ОЗУ ->
# вернуть main.c -> собрать -> прошить обратно рабочую версию.
# Архив на флеш при этом только читается.
#
#   ./tools/read_data.sh [выходной_файл]      (по умолчанию build/data.txt)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="${1:-$ROOT/build/data.txt}"
mkdir -p "$(dirname "$OUT")"

PLUGINS="/e/ProgramData/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins"
PROG="$PLUGINS/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304/tools/bin/STM32_Programmer_CLI.exe"
ELF_W="$(cygpath -w "$ROOT/Debug/FieldSensor.elf")"
BACKUP="$(mktemp)"
TMPBIN="$(mktemp -u).bin"

restore_src() {
    if [ -f "$BACKUP" ]; then
        cp "$BACKUP" "$ROOT/Core/Src/main.c"
        rm -f "$BACKUP"
        echo ">> main.c восстановлен"
    fi
}

# На плате в этот момент может лежать дампер — прошивка, которая намеренно висит
# в while(1) и ничего не пишет. Оставить её там после аварийного выхода значит
# молча остановить сбор данных, причём со стороны плата выглядит рабочей.
# Поэтому рабочая версия возвращается при любом исходе, а не только при успехе.
on_exit() {
    local code=$?
    restore_src
    if [ "${FLASHED_DUMPER:-0}" = "1" ] && [ "${RESTORED_FW:-0}" != "1" ]; then
        echo ">> аварийный откат: возвращаю рабочую прошивку" >&2
        if "$ROOT/tools/dev.sh" build >/dev/null 2>&1 && flash_now; then
            echo ">> рабочая прошивка возвращена" >&2
        else
            echo "!! НЕ УДАЛОСЬ. Прошейте вручную: ./tools/dev.sh flash" >&2
        fi
    fi
    exit $code
}
trap on_exit EXIT

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

flash_now() {
    retry 20 "verified successfully" -c port=SWD mode=UR -w "$ELF_W" -v -rst >/dev/null
}

echo ">> 1/6 сохраняю main.c"
cp "$ROOT/Core/Src/main.c" "$BACKUP"

echo ">> 2/6 вставляю дампер и собираю"
python tools/dump_patch.py
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 3/6 прошиваю дампер"
flash_now
FLASHED_DUMPER=1

echo ">> 4/6 читаю ОЗУ"
# Адреса берём из карты линковки, а не прибиваем константами: они уезжают от
# любой правки, а молча прочитанный не тот кусок ОЗУ выглядел бы как испорченные
# данные, а не как ошибка.
BUF=$(grep -m1 '\.bss\.dump_buf' Debug/FieldSensor.map | awk '{print $2}')
MAG=$(grep -A1 -m1 '\.bss\.dump_magic' Debug/FieldSensor.map | tail -1 | awk '{print $1}')

# Строка ответа выглядит так: "0x20009398 : D00DFEED 000001B9 000001B9"
words=$(retry 20 " : " -c port=SWD mode=HOTPLUG -r32 "$MAG" 12 \
        | grep -E "^0x[0-9A-Fa-f]+ : " | tail -1)
magic=$(echo "$words" | awk '{print $3}')
fsize=$((16#$(echo "$words" | awk '{print $4}')))
len=$((16#$(echo "$words"   | awk '{print $5}')))

[ "$magic" = "D00DFEED" ] || { echo "дамп не выполнился (magic=$magic)" >&2; exit 1; }
echo "   файл на флеш: $fsize Б, снято: $len Б"

if [ "$len" -gt 0 ]; then
    # Программатор читает только кратно четырём байтам, поэтому лишний хвост
    # отрезаем уже на стороне компьютера.
    rounded=$(( (len + 3) / 4 * 4 ))
    retry 20 "read successfully" \
        -c port=SWD mode=HOTPLUG -r "$BUF" "$rounded" "$(cygpath -w "$TMPBIN")" >/dev/null
    python -c "import sys; open(sys.argv[1],'wb').write(open(sys.argv[2],'rb').read()[:int(sys.argv[3])])" \
        "$OUT" "$TMPBIN" "$len"
    rm -f "$TMPBIN"
    echo "   записано: $OUT"
else
    echo "   файл на плате пуст" >&2
fi

echo ">> 5/6 возвращаю рабочую версию"
restore_src
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 6/6 прошиваю рабочую версию обратно"
flash_now
RESTORED_FW=1
echo ">> готово"
