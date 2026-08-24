#!/usr/bin/env bash
# Сборка и прошивка FieldSensor без запуска CubeIDE.
#
# Всё нужное уже лежит внутри установленной CubeIDE — отдельный toolchain
# ставить не требуется. Пути прибиты к конкретной версии IDE: если она
# обновится, поправить PLUGINS ниже (каталоги содержат версию в имени).
#
#   ./tools/dev.sh build      — собрать (Debug)
#   ./tools/dev.sh rebuild    — пересобрать с нуля
#   ./tools/dev.sh flash      — собрать и залить, оставив плату работать
#   ./tools/dev.sh reset      — перезапустить плату
#   ./tools/dev.sh size       — размер прошивки
#   ./tools/dev.sh connect    — проверить связь с платой
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGINS="/e/ProgramData/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins"
GCC_BIN="$PLUGINS/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin"
MAKE_BIN="$PLUGINS/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.200.202604021615/tools/bin"
PROG="$PLUGINS/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304/tools/bin/STM32_Programmer_CLI.exe"
ELF="$ROOT/Debug/FieldSensor.elf"

export PATH="$GCC_BIN:$MAKE_BIN:$PATH"

# Плата почти всё время лежит в STOP, а SWD там обесточен, поэтому попасть в
# неё можно только в короткое окно бодрствования. Отсюда повторы: это не
# признак неисправности, а нормальная плата за режим сна.
prog_retry() {
    local attempts=$1; shift
    local i out
    for ((i = 1; i <= attempts; i++)); do
        if out=$("$PROG" "$@" 2>&1) && echo "$out" | grep -q "Device ID"; then
            echo "$out"; return 0
        fi
        echo "  попытка $i/$attempts: плата спит, повтор..." >&2
        # Пауза обязательна. Без неё попытки идут вплотную, период их повторения
        # оказывается сравним с периодом сна платы, и они раз за разом попадают
        # в одну и ту же фазу — в сон. С паузой фаза уползает и окно ловится.
        sleep 5
    done
    echo "$out" >&2
    echo "Не удалось поймать окно бодрствования за $attempts попыток." >&2
    return 1
}

# Полная строка вызова компилятора — это полсотни ключей и два десятка -I,
# и таких строк на пересборку шестьдесят. Читать в них нечего, а места они
# занимают столько, что за ними теряется единственное, что важно: предупреждения,
# ошибки и итоговый размер. Поэтому по умолчанию наружу идут только они.
# Полный вывод: VERBOSE=1 ./tools/dev.sh build
build_quiet() {
    if [ "${VERBOSE:-0}" = "1" ]; then
        make -C "$ROOT/Debug" -j8 all
        return
    fi
    local log; log="$(mktemp)"
    if make -C "$ROOT/Debug" -j8 all >"$log" 2>&1; then
        grep -E "warning:|error:" "$log" || true
        "$GCC_BIN/arm-none-eabi-size.exe" "$ELF"
        rm -f "$log"
    else
        echo "--- сборка упала, полный вывод: ---" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi
}

case "${1:-build}" in
  build)   build_quiet ;;
  rebuild) make -C "$ROOT/Debug" clean >/dev/null && build_quiet ;;
  size)    "$GCC_BIN/arm-none-eabi-size.exe" "$ELF" ;;
  connect) prog_retry 20 -c port=SWD mode=UR | grep -Ei "Device ID|Revision|CPU" ;;
  # -rst в конце обязателен: без него программатор оставляет ядро остановленным,
  # плата не спит и не пишет данные, а выглядит это как «прошилось и умерло».
  reset)   prog_retry 20 -c port=SWD mode=UR -rst | grep -Ei "Device ID|reset" ;;
  flash)
      # Часы платы встают на время компиляции ИМЕННО time_service.c: BUILD_STAMP
      # там собран из __DATE__/__TIME__, а они подставляются в момент компиляции
      # того файла, где написаны. Time_SyncToBuildIfNeeded переставляет часы
      # только когда стамп изменился — значит правка любого ДРУГОГО файла новую
      # прошивку заливает, а часы не трогает, и они молча остаются от прошлой.
      # Прибор при этом выглядит исправным, а метки времени врут. Поэтому перед
      # прошивкой стамп обновляем принудительно: прошивка = новая сборка = часы
      # заново. Снятие данных (read_data.sh) сюда не попадает и часы не сдвигает.
      #
      # Остаётся смещение на время ожидания окна: собрали, потом до часа ловили
      # спящую плату. Убирается только тем, что человек нажмёт KEY — тогда
      # прошивка ложится через секунды после сборки. Иначе смещение равно
      # разнице между временем прошивки и mtime Debug/Core/Src/time_service.o.
      touch "$ROOT/Core/Src/time_service.c"
      make -C "$ROOT/Debug" -j8 all
      prog_retry 20 -c port=SWD mode=UR -w "$(cygpath -w "$ELF")" -v -rst \
          | grep -Ei "Device ID|Download|verified|Error|reset"
      ;;
  *) echo "Неизвестная команда: $1" >&2; exit 2 ;;
esac
