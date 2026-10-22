#!/bin/sh
# Проверка синтаксиса и типов прошивки обычным хостовым gcc, без ARM-тулчейна.
# Это НЕ замена сборке в CubeIDE (./tools/dev.sh build): кодогенерация, размер
# и ассемблерные вставки здесь не проверяются. Зато ловит опечатки, неверные
# типы и необъявленные функции там, где ARM-тулчейна нет (облачные сессии).
# Падает только на ошибках; предупреждения печатает.
cd "$(dirname "$0")/.." || exit 2
INC="-ICore/Inc -IDrivers/STM32F4xx_HAL_Driver/Inc -IDrivers/CMSIS/Device/ST/STM32F4xx/Include -IDrivers/CMSIS/Include -IFATFS/App -IFATFS/Target -IMiddlewares/Third_Party/FatFs/src -IUSB_DEVICE/App -IUSB_DEVICE/Target -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc -IMiddlewares/ST/STM32_USB_Device_Library/Class/MSC/Inc"
# Хост 64-битный: приведения указатель<->uint32_t и переполнения констант CMSIS
# здесь шумят, а на Cortex-M4 корректны.
QUIET="-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-overflow"
fail=0
for f in Core/Src/*.c FATFS/Target/user_diskio.c FATFS/App/fatfs.c USB_DEVICE/App/*.c; do
  case "$f" in */syscalls.c|*/sysmem.c) continue;; esac
  out=$(gcc -fsyntax-only -std=gnu11 -DSTM32F401xE -DUSE_HAL_DRIVER $INC -Wall $QUIET "$f" 2>&1 | grep -E "error|warning")
  [ -n "$out" ] && { echo "== $f"; echo "$out"; }
  echo "$out" | grep -q "error" && fail=1
done
sh tools/cubemx_guard.sh || fail=1
[ $fail -eq 0 ] && echo "syncheck: OK (ошибок нет)" || echo "syncheck: ЕСТЬ ОШИБКИ"
exit $fail
