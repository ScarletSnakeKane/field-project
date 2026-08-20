#!/usr/bin/env bash
# Снять архив data.txt с платы через ST-Link, не подключая её по USB.
#
# Цикл: сохранить main.c -> вставить дампер -> собрать -> прошить -> вычитать
# ОЗУ -> вернуть main.c -> собрать -> прошить обратно рабочую версию.
# Архив на флеш только читается. Результат кладётся в build/data.txt.
#
#   ./tools/read_data.sh [выходной_файл]
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

restore() {
    if [ -f "$BACKUP" ]; then
        cp "$BACKUP" "$ROOT/Core/Src/main.c"
        rm -f "$BACKUP"
        echo ">> main.c восстановлен"
    fi
}
trap restore EXIT

# Плата почти всё время в STOP, где SWD обесточен, поэтому в неё попадают
# только в окно бодрствования — отсюда повторы.
retry() {
    local n=$1 need=$2; shift 2
    local i out
    for ((i = 1; i <= n; i++)); do
        out=$("$PROG" "$@" 2>&1 || true)
        if echo "$out" | grep -q "$need"; then echo "$out"; return 0; fi
    done
    echo "$out" >&2; echo "не удалось за $n попыток: $need" >&2; return 1
}

flash_now() { retry 20 "verified successfully" -c port=SWD mode=UR -w "$ELF_W" -v -rst >/dev/null; }

echo ">> 1/6 сохраняю main.c"
cp "$ROOT/Core/Src/main.c" "$BACKUP"

echo ">> 2/6 вставляю дампер и собираю"
python tools/dump_patch.py
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 3/6 прошиваю дампер"
flash_now

echo ">> 4/6 читаю ОЗУ"
BUF=$(grep -m1 '\.bss\.dump_buf'  Debug/FieldSensor.map | awk '{print $2}')
MAG=$(grep -A1 -m1 '\.bss\.dump_magic' Debug/FieldSensor.map | tail -1 | awk '{print $1}')
words=$(retry 20 "0x" -c port=SWD mode=HOTPLUG -r32 "$MAG" 12 | grep -o '0x[0-9A-Fa-f]*  *[0-9A-F]* [0-9A-F]* [0-9A-F]*' | tail -1)
magic=$(echo "$words" | awk '{print $2}'); fsize=$((16#$(echo "$words" | awk '{print $3}'))); len=$((16#$(echo "$words" | awk '{print $4}')))
[ "$magic" = "D00DFEED" ] || { echo "дамп не выполнился (magic=$magic)" >&2; exit 1; }
echo "   файл на флеш: $fsize Б, снято: $len Б"
if [ "$len" -gt 0 ]; then
    rounded=$(( (len + 3) / 4 * 4 ))
    retry 20 "read successfully" -c port=SWD mode=HOTPLUG -r "$BUF" "$rounded" "$(cygpath -w "$TMPBIN")" >/dev/null
    python -c "import sys; open(sys.argv[1],'wb').write(open(sys.argv[2],'rb').read()[:int(sys.argv[3])])" "$OUT" "$TMPBIN" "$len"
    rm -f "$TMPBIN"
    echo "   записано: $OUT"
else
    echo "   файл на плате пуст" >&2
fi

echo ">> 5/6 возвращаю рабочую версию"
restore
"$ROOT/tools/dev.sh" build >/dev/null

echo ">> 6/6 прошиваю рабочую версию обратно"
flash_now
echo ">> готово"
