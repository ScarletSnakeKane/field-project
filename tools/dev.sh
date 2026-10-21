#!/usr/bin/env bash
# Сборка и прошивка FieldSensor без запуска CubeIDE.
#
# Всё нужное уже лежит внутри установленной CubeIDE — отдельный toolchain
# ставить не требуется. Где искать CubeIDE — см. tools/cubeide.sh
# (CUBEIDE_PLUGINS, STM32_PROG).
#
#   ./tools/dev.sh build      — собрать (Debug)
#   ./tools/dev.sh rebuild    — пересобрать с нуля
#   ./tools/dev.sh flash      — собрать и залить, оставив плату работать
#   ./tools/dev.sh reset      — перезапустить плату
#   ./tools/dev.sh size       — размер прошивки
#   ./tools/dev.sh connect    — проверить связь с платой
#
# flash/reset/connect ждут окно бодрствования спящей платы до PROG_WAIT_SEC
# секунд (по умолчанию 4200 — больше часового интервала). Сразу — если
# воткнуть USB самой платы или держать KEY на работающей плате.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/tools/cubeide.sh"
GCC_BIN="$(plugin_bin 'com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*.win32_*')"
MAKE_BIN="$(plugin_bin 'com.st.stm32cube.ide.mcu.externaltools.make.win32_*')"
ELF="$ROOT/Debug/FieldSensor.elf"

export PATH="$GCC_BIN:$MAKE_BIN:$PATH"

# Раньше здесь было 20 попыток с паузой 5 с — около двух минут, тогда как окно
# бодрствования при часовом интервале длится полторы секунды раз в час. Окно
# ловилось с вероятностью порядка 5 %, а именно этой командой read_data.sh
# советует возвращать рабочую прошивку после неудачного отката (D7). Теперь
# ждём по сроку, как read_data.sh, и не прячем ошибку программатора.
PROG_WAIT_SEC="${PROG_WAIT_SEC:-4200}"
# prog_retry <маркер успеха> <аргументы программатора...>
prog_retry() {
    local need=$1; shift
    require_prog
    require_stlink
    prog_catch "$PROG_WAIT_SEC" "$need" "$@" || exit $?
    echo "$PROG_OUT"
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
  connect) prog_retry "Device ID" -c port=SWD mode=UR | grep -Ei "Device ID|Revision|CPU" ;;
  # -rst в конце обязателен: без него программатор оставляет ядро остановленным,
  # плата не спит и не пишет данные, а выглядит это как «прошилось и умерло».
  reset)   prog_retry "Device ID" -c port=SWD mode=UR -rst | grep -Ei "Device ID|reset" ;;
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
      # Успех — только «verified successfully»: «Device ID» печатается уже при
      # подключении, и прерванная запись прошла бы за удачную.
      prog_retry "verified successfully" -c port=SWD mode=UR -w "$(cygpath -w "$ELF")" -v -rst \
          | grep -Ei "Device ID|Download|verified|Error|reset"
      ;;
  *) echo "Неизвестная команда: $1" >&2; exit 2 ;;
esac
