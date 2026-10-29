#!/bin/sh
# Сторож правок вне USER CODE, которые CubeMX молча затрёт при «Generate Code».
# Каждая из них закрывает находку аудита; после перегенерации прошивка собирается
# и проходит хостовые тесты, а на плату уходит регресс. Нужны только sh/grep/awk —
# работает в Git Bash у владельца без gcc. Вызывается из dev.sh build,
# syncheck.sh и tools/hooks/pre-commit. Падает с перечнем того, что вернуть.
cd "$(dirname "$0")/.." || exit 2
fail=0
bad() { echo "CubeMX откатил правку: $1"; echo "  вернуть: $2"; fail=1; }
# Для правок, которые CubeMX не трогает, но которые разводят между собой
# решения в main.c: сообщение другое, потому что виноват не генератор.
wire() { echo "Проводка USB-сессии разошлась: $1"; echo "  как надо: $2"; fail=1; }

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

# --- Проводка USB-сессии в main.c -------------------------------------------
# Сюда хостовые тесты не достают: main.c в них не компилируется, и правила
# power_policy.c можно сколько угодно проверять по отдельности — связку между
# ними держит только этот файл. Проверки текстовые, зато они ловят ровно те
# правки, которыми дефект воспроизводится дословно.

# Признак USB-сессии — ОДИН на все три решения: usb_started (стек поднят).
# По dev_state == CONFIGURED они расходятся в SUSPENDED (штатная середина
# сессии у Android): уступка не вооружается, счёт пропусков обнуляется каждый
# час, а строка уходит в очередь в ОЗУ — прибор бодрствует непрерывно с
# пустым архивом. Это не теория: так и было (до 100 % бодрствования, 0 строк
# за двое суток по модели).
if ! awk '/Policy_SkipMeasureForUsb\(&usb_policy/{n=NR}
          n && NR<=n+2 && /usb_started/{f=1} END{exit !f}' "$MAIN"; then
    wire "признак сессии — решение о пропуске замера получает не usb_started" \
        "Policy_SkipMeasureForUsb(&usb_policy, woke_by_vbus, woke_by_button, usb_started, USB_SESSION_MAX_SKIPS)"
fi
grep -q 'Policy_SessionYieldsToMeasure(&usb_policy, usb_started' "$MAIN" || \
    wire "признак сессии — уступка замеру получает не usb_started" \
        "Policy_SessionYieldsToMeasure(&usb_policy, usb_started, USB_SESSION_MAX_SKIPS)"
# Смотреть на CONFIGURED вправе только цикл удержания (usb_config для
# Policy_UsbKeepsAwake): ему состояние хоста нужно для потолка ожидания
# перечисления. Любое второе вхождение — это снова разошедшийся признак.
if [ "$(grep -c 'USBD_STATE_CONFIGURED' "$MAIN")" != "1" ]; then
    wire "dev_state == CONFIGURED в main.c больше одного раза" \
        "единственное место — usb_config в цикле удержания; решения о пропуске, уступке и о «на флеш или в ОЗУ» берут usb_started"
fi
# Третье решение — «на флеш или в очередь в ОЗУ» в блоке записи замера. Там
# своя табуляция (тело главного цикла), по ней и отличаем его от одноимённого
# условия в разборке перед сном.
grep -q "^	  if (usb_started)$" "$MAIN" || \
    wire "ветка «на флеш или в очередь в ОЗУ» больше не смотрит на usb_started" \
        "в блоке записи замера условие очереди — if (usb_started), тем же признаком, что пропуск и уступка"

# Р5: сначала уйти с шины, потом усыплять носитель. Пока стек жив, хост
# вправе прислать READ10, а усыплённая флеш отвечает на него USBD_FAIL, то
# есть хост получает STALL и ошибку ввода-вывода вместо чистого отключения.
awk '/SPI_Flash_DeepPowerDown\(\);/{d=1} d && /Usb_Stop\(\);/{f=1} END{exit f}' "$MAIN" || \
    wire "Usb_Stop() стоит ПОСЛЕ SPI_Flash_DeepPowerDown()" \
        "в SleepUntilNextSample гасить стек ДО усыпления флеш: иначе чтение хоста в это окно получает STALL"
# Мало того, что Usb_Stop() не ПОСЛЕ усыпления: он обязан быть и в самой
# разборке перед сном. Второй Usb_Stop() в этой функции — внутри цикла
# удержания (уступка замеру), он глубже по отступу и разборку не заменяет:
# без неё плата уходит в STOP с поднятым стеком. Отличаем по отступу ровно в
# шесть пробелов — тела функции. Переформатировали — сторож сработает, и это
# правильнее, чем пропустить потерянную разборку.
awk '/^      Usb_Stop\(\);/ && !d {u=1} /SPI_Flash_DeepPowerDown\(\);/{d=1} END{exit !(u && d)}' "$MAIN" || \
    wire "в разборке перед сном не осталось Usb_Stop() (на уровне тела функции, до усыпления флеш)" \
        "в SleepUntilNextSample: if (usb_started) Usb_Stop(); — ДО SPI_Flash_DeepPowerDown()"

[ $fail -eq 0 ] && echo "cubemx_guard: OK"
exit $fail
