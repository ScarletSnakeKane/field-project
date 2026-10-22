#!/bin/sh
# Сторож правок вне USER CODE, которые CubeMX молча затрёт при «Generate Code».
# Каждая из них закрывает находку аудита; после перегенерации прошивка собирается
# и проходит хостовые тесты, а на плату уходит регресс. Нужны только sh/grep/awk —
# работает в Git Bash у владельца без gcc. Вызывается из dev.sh build,
# syncheck.sh и tools/hooks/pre-commit. Падает с перечнем того, что вернуть.
cd "$(dirname "$0")/.." || exit 2
fail=0
bad() { echo "CubeMX откатил правку: $1"; echo "  вернуть: $2"; fail=1; }

MAIN=Core/Src/main.c
BOT=Middlewares/ST/STM32_USB_Device_Library/Class/MSC/Src/usbd_msc_bot.c
CONF=Core/Inc/stm32f4xx_hal_conf.h

# A3: USB поднимается только после записи цикла, не при инициализации периферии.
if awk '/Initialize all configured peripherals/{s=1} /USER CODE BEGIN 2/{s=0}
        s && /^[[:space:]]*MX_USB_DEVICE_Init\(\);/{f=1} END{exit !f}' "$MAIN"; then
    bad "A3 — вызов MX_USB_DEVICE_Init() вернулся в начало main()" \
        "удалить вызов; в FieldSensor.ioc: Project Manager → Advanced Settings → MX_USB_DEVICE_Init → «Do not generate function call»"
fi

# A5: LSE запускает MX_RTC_Init с откатом на LSI, а не SystemClock_Config.
if awk '/^void SystemClock_Config\(void\)/{s=1} s && /^}/{s=0}
        s && /RCC_OSCILLATORTYPE_LSE/ && !/^[[:space:]]*(\/\/|\/\*|\*)/{f=1} END{exit !f}' "$MAIN"; then
    bad "A5 — LSE снова поднимается в SystemClock_Config (отказ кварца → цикл перезагрузок)" \
        "OscillatorType = RCC_OSCILLATORTYPE_HSI, без LSEState (см. комментарий «LSE отсюда убран (A5)»)"
fi

# A5: ожидание LSE — 10 с, не 5.
grep -q 'LSE_STARTUP_TIMEOUT[[:space:]]*10000U' "$CONF" || \
    bad "A5 — LSE_STARTUP_TIMEOUT не 10000" "в $CONF и в .ioc: RCC → LSE Startup Timeout = 10000"

# V1 и сброс BOT: правки в библиотеке USB ST (LibraryCopy=1 перезаписывает Middlewares).
grep -q 'FieldSensor (П5б+, V1)' "$BOT" || \
    bad "V1 — отказ чтения посреди READ10 снова шлёт CSW без STALL (хост висит ~30 с)" \
        "git checkout -p -- $BOT (кусок в MSC_BOT_DataIn с пометкой «FieldSensor (П5б+, V1)»)"
grep -q 'FieldSensor (приёмка П5б+)' "$BOT" || \
    bad "сброс BOT — после Bulk-Only Reset класс перестаёт слать CSW" \
        "git checkout -p -- $BOT (строка bot_status = USBD_BOT_STATUS_NORMAL в MSC_BOT_CBW_Decode)"

[ $fail -eq 0 ] && echo "cubemx_guard: OK"
exit $fail
