# Общее для dev.sh и read_data.sh: где лежит CubeIDE и как ловить окно
# бодрствования спящей платы. Подключается через source, сам не запускается.
#
# Пути раньше были прибиты к одной установке (/e/ProgramData/..., версия 2.2.0).
# На машине, где CubeIDE стоит в C:\ST или обновилась, программатор просто не
# находился, а скрипт этого не замечал и 70 минут «ловил окно» (D7). Теперь:
#   CUBEIDE_PLUGINS=<каталог plugins внутри CubeIDE>  — задать явно;
#   STM32_PROG=<путь к STM32_Programmer_CLI.exe>      — задать программатор;
# иначе берётся прежний путь, а если его нет — ищется любая установленная версия.

find_plugins() {
    local def="/e/ProgramData/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins"
    if [ -n "${CUBEIDE_PLUGINS:-}" ]; then echo "$CUBEIDE_PLUGINS"; return; fi
    if [ -d "$def" ]; then echo "$def"; return; fi
    local d found=""
    # Маска раскрывается по алфавиту, так что последней остаётся самая новая версия.
    for d in /?/ProgramData/ST/STM32CubeIDE_*/STM32CubeIDE/plugins \
             /?/ST/STM32CubeIDE_*/STM32CubeIDE/plugins; do
        [ -d "$d" ] && found="$d"
    done
    echo "${found:-$def}"
}

# Каталог tools/bin плагина по маске имени: в имени плагина сидит версия.
plugin_bin() {
    local d found=""
    for d in "$PLUGINS"/$1/tools/bin; do
        [ -d "$d" ] && found="$d"
    done
    echo "$found"
}

PLUGINS="$(find_plugins)"
PROG="${STM32_PROG:-$(plugin_bin 'com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*')/STM32_Programmer_CLI.exe}"

# Ошибки, которые повтором не лечатся: ждать окно при них бессмысленно.
# Точные формулировки программатора сверены не на всех версиях, поэтому
# остальные ошибки не считаются постоянными, но печатаются по ходу ожидания.
PROG_PERMANENT='no debug probe|no st-?link|st-?link.*not (found|detected)|does not exist|no such file|cannot open file|file not found'

require_prog() {
    if [ ! -f "$PROG" ]; then
        echo "!! не найден программатор: $PROG" >&2
        echo "   Укажите каталог plugins установленной CubeIDE: CUBEIDE_PLUGINS=/c/ST/STM32CubeIDE_x.y.z/STM32CubeIDE/plugins" >&2
        echo "   или сам программатор: STM32_PROG=/путь/к/STM32_Programmer_CLI.exe" >&2
        exit 2
    fi
}

# Есть ли вообще ST-Link. Без него ожидание окна длилось бы до часа впустую.
require_stlink() {
    local out
    out=$("$PROG" -l st 2>&1 | tr '\r' '\n' || true)
    if echo "$out" | grep -qiE "$PROG_PERMANENT"; then
        echo "$out" | grep -iE "error|st-?link|probe" | tail -3 >&2
        echo "!! ST-Link не найден: проверьте кабель и что CubeIDE не держит отладчик" >&2
        exit 2
    fi
}

WAKE_HINT="ускорить: воткните USB самой платы в компьютер (VBUS не даёт ей заснуть) или нажмите и держите KEY на уже работающей плате"

# Повторять команду программатора, пока в выводе не появится маркер успеха.
#   prog_catch <секунд> <маркер> <аргументы программатора...>
# Плата почти всё время лежит в STOP, где SWD обесточен; окно бодрствования при
# часовом интервале — около полутора секунд, так что ждать приходится до часа,
# а пробовать — раз в секунду, чтобы не проскочить окно. Вывод программатора
# не глотается: последняя ошибка печатается по ходу, и «нет ST-Link» не
# прячется за «не поймал окно».
# Возврат: 0 — успех (вывод в PROG_OUT), 1 — время вышло, 2 — постоянная ошибка.
prog_catch() {
    local wait=$1 need=$2; shift 2
    local start now out err last="" shown=0
    start=$(date +%s)
    PROG_OUT=""
    echo "   жду окно бодрствования (до $wait с); $WAKE_HINT" >&2
    while :; do
        out=$("$PROG" "$@" 2>&1 | tr '\r' '\n' || true)
        if echo "$out" | grep -q "$need"; then
            PROG_OUT=$out
            return 0
        fi
        err=$(echo "$out" | grep -iE "error|fail" | tail -1 || true)
        if echo "$out" | grep -qiE "$PROG_PERMANENT"; then
            echo "$out" | tail -5 >&2
            echo "!! постоянная ошибка, повтор не поможет: ${err:-см. вывод выше}" >&2
            return 2
        fi
        now=$(date +%s)
        if [ "$now" -ge $((start + wait)) ]; then
            break
        fi
        if [ "$err" != "$last" ] || [ $((now - shown)) -ge 60 ]; then
            echo "   прошло $((now - start)) с; программатор: ${err:-без сообщения об ошибке}" >&2
            last=$err
            shown=$now
        fi
        sleep 1
    done
    echo "$out" | tail -5 >&2
    echo "не удалось поймать окно бодрствования за $wait с; $WAKE_HINT" >&2
    return 1
}
