#include "power_policy.h"

#include <string.h>

uint8_t Policy_ButtonKeepsAwake(Policy_Button *b, uint8_t key_held,
                                uint32_t dt_ms, uint32_t hold_max_ms)
{
    if (!key_held)
    {
        /* Отпустили — залипание (если было) кончилось, следующее нажатие
         * снова честное. */
        b->stuck   = 0;
        b->held_ms = 0;
        return 0U;
    }

    /* Признанная залипшей кнопка больше не держит: счёт стоит на потолке и
     * обнулится только отпусканием. Без переполнения: удержание длиннее
     * 49 суток упирается в потолок, а не начинается с нуля. */
    b->held_ms = (dt_ms > UINT32_MAX - b->held_ms) ? UINT32_MAX : b->held_ms + dt_ms;
    if (b->held_ms >= hold_max_ms)
    {
        b->stuck = 1;
        return 0U;
    }
    return 1U;
}

/* Прибавка, которая упирается в край uint32, а не начинается с нуля:
 * удержание длиннее 49 суток должно упираться в потолок. */
static uint32_t sat_add(uint32_t a, uint32_t b)
{
    return (b > UINT32_MAX - a) ? UINT32_MAX : a + b;
}

uint8_t Policy_UsbKeepsAwake(Policy_Usb *u, uint8_t usb_present, uint8_t usb_stack_up,
                             uint8_t usb_configured, uint8_t vbus_edge, uint32_t host_blocks,
                             uint32_t dt_ms, uint32_t enum_max_ms, uint32_t idle_max_ms)
{
    /* Хост получил данные с прошлого вызова — единственный признак, что он
     * действительно на месте. Счёт ведём здесь, а не в main.c: это правило,
     * а правила живут под тестами. */
    uint8_t host_active = (host_blocks != u->host_blocks);
    u->host_blocks = host_blocks;

    /* Кабель вынули — забываем про этот VBUS всё, включая израсходованный
     * шанс. Вставили заново — то же самое, но только если сессии сейчас нет:
     * во время сессии фронт на входе VBUS — это дребезг, и права продлевать
     * её у него нет (иначе граница сессии опять оказывается не наша). */
    if (!usb_present || (vbus_edge && !usb_stack_up))
    {
        u->unconfigured_ms = 0;
        u->idle_ms         = 0;
        u->enumerated      = 0;
        u->spent           = 0;
    }

    /* Начало сессии — подъём стека нашей прошивкой, и ничто другое. Счёт
     * пропущенных интервалов (skipped) здесь не трогаем: он принадлежит
     * расписанию замеров, а не сессии, и обнуляется только состоявшимся
     * замером. Иначе дребезг VBUS или хост, перечисляющийся заново каждый
     * час, выдавали бы себе новое право на пропуск бесконечно. */
    if (usb_stack_up && !u->stack_up)
    {
        u->unconfigured_ms = 0;
        u->idle_ms         = 0;
        u->enumerated      = 0;
    }
    u->stack_up = usb_stack_up ? 1U : 0U;

    if (!usb_present)
        return 0U;

    /* Шанс израсходован: ни держать, ни поднимать стек — до пропадания или
     * нового фронта VBUS. */
    if (u->spent)
        return 0U;

    /* Перечисление доказывают и отданные секторы, а не только dev_state в
     * момент опроса. Зарядка их не выпросит: READ10 приходит лишь от хоста,
     * который плату перечислил. А вот обратное неверно — при строгом
     * selective suspend окно CONFIGURED короче шага опроса (100 мс), и хост,
     * уже получивший сотни секторов, по одному dev_state объявлялся бы
     * зарядкой: через enum_max_ms шанс помечался израсходованным и стек
     * гас посреди чтения. Та же болезнь, что и расхождение признака сессии:
     * живость хоста меряется данными, а не его состоянием. */
    if (usb_configured || host_active)
        u->enumerated = 1;

    /* Хост ещё не перечислялся — ждём не дольше enum_max_ms, иначе это
     * зарядка, пауэрбанк или влага на делителе. */
    if (!u->enumerated)
    {
        u->unconfigured_ms = sat_add(u->unconfigured_ms, dt_ms);
        if (u->unconfigured_ms >= enum_max_ms)
        {
            u->spent = 1;
            return 0U;
        }
        return 1U;
    }

    /* Сессия идёт. Порция данных — сессия живая, потолок молчания начинается
     * заново; без этого обнуления чтение архива рвалось бы на середине. */
    if (host_active)
    {
        u->idle_ms = 0;
        return 1U;
    }

    u->idle_ms = sat_add(u->idle_ms, dt_ms);
    if (u->idle_ms >= idle_max_ms)
    {
        u->spent = 1;
        return 0U;
    }
    return 1U;
}

uint8_t Policy_SessionYieldsToMeasure(Policy_Usb *u, uint8_t usb_session, uint32_t max_skips)
{
    if (!usb_session || u->skipped < max_skips)
        return 0U;

    /* Сессия уступает — и её шанс на этом кончился: иначе стек поднялся бы
     * снова на следующем же пробуждении, и съеденные интервалы пошли бы по
     * кругу. */
    u->spent = 1;
    return 1U;
}

uint8_t Policy_MeasureDue(uint32_t awake_ms, uint32_t interval_ms)
{
    return (awake_ms >= interval_ms) ? 1U : 0U;
}

uint8_t Policy_SkipMeasureForUsb(Policy_Usb *u, uint8_t woke_by_vbus, uint8_t woke_by_button,
                                 uint8_t usb_session, uint32_t max_skips)
{
    /* Причины пропуска две — идёт сессия и плату разбудил вставленный кабель
     * (а не кнопка), — но потолок у них один. Отдельно взятая причина права
     * пропускать бесконечно не имеет: на фронтах VBUS без хоста сессии не
     * возникает вовсе, и ограничить их через Policy_SessionYieldsToMeasure
     * нечем. */
    if (!usb_session && !(woke_by_vbus && !woke_by_button))
        return 0U;

    if (u->skipped >= max_skips)
        return 0U;   /* право исчерпано — меряем, что бы там ни было с USB */

    u->skipped++;
    return 1U;
}

void Policy_MeasureStored(Policy_Usb *u, uint8_t stored)
{
    if (stored)
        u->skipped = 0;
}

uint32_t Policy_SleepChunkSec(uint32_t left_sec, uint32_t kick_sec)
{
    if (kick_sec == 0U || left_sec < kick_sec)
        return left_sec;
    return kick_sec;
}

uint8_t Policy_CounterMoves(Policy_ReadCounter read, void *ctx, uint32_t polls)
{
    uint32_t first = read(ctx);
    for (uint32_t i = 1U; i < polls; i++)
    {
        if (read(ctx) != first)
            return 1;
    }
    return 0;
}

uint16_t Policy_ResetCause(uint32_t rcc_csr, uint32_t bkp_fault)
{
    uint16_t c = 0;

    if (rcc_csr & POLICY_CSR_PORRSTF)  c |= 1U;
    if (rcc_csr & POLICY_CSR_PINRSTF)  c |= 2U;
    if (rcc_csr & POLICY_CSR_BORRSTF)  c |= 4U;
    if (rcc_csr & POLICY_CSR_SFTRSTF)  c |= 8U;
    if (rcc_csr & POLICY_CSR_IWDGRSTF) c |= 16U;
    if (rcc_csr & POLICY_CSR_WWDGRSTF) c |= 32U;
    if (rcc_csr & POLICY_CSR_LPWRRSTF) c |= 64U;

    /* Метка не та — это ноль после потери питания или мусор, а не наш код.
     * Код без программного сброса или сброса сторожем тоже не в счёт:
     * обработчики исключений сбрасывают программно, Error_Handler — сторожем,
     * а при другом сбросе код остался от прошлого раза. */
    if ((bkp_fault & 0xFFFF0000UL) == POLICY_FAULT_MAGIC &&
        (rcc_csr & (POLICY_CSR_SFTRSTF | POLICY_CSR_IWDGRSTF)))
    {
        uint32_t code = bkp_fault & 0xFFFFUL;
        if (code >= 1U && code <= 15U)
            c |= (uint16_t)(code << 8);
    }
    return c;
}

uint8_t Policy_SensorsPowerAllowed(const Policy_Quarantine *q)
{
    return (q->skip_left == 0U) ? 1U : 0U;
}

void Policy_QuarantineUpdate(Policy_Quarantine *q, uint8_t powered,
                             uint8_t all_failed, uint32_t skip_cycles)
{
    if (!powered)
    {
        if (q->skip_left > 0U)
            q->skip_left--;
        return;
    }

    if (!all_failed)
    {
        q->fails = 0;
        return;
    }

    /* Счётчик не сбрасывается после карантина: пробная попытка, которая
     * тоже не удалась, сразу возвращает в карантин, а не даёт ещё два цикла
     * на полном токе. */
    if (q->fails < POLICY_QUARANTINE_FAILS)
        q->fails++;
    if (q->fails >= POLICY_QUARANTINE_FAILS)
        q->skip_left = skip_cycles;
}

uint8_t Policy_TimeTrusted(uint8_t sync, uint8_t stored, uint8_t archive_read,
                           const char *last_ts, const char *build_ts)
{
    if (sync == POLICY_TIME_SYNC_NONE)
        return stored ? 1U : 0U;

    /* Новая сборка при живом календаре — обычно прошивка с компьютера: время
     * сборки отстаёт от настоящего на минуты. Но «новая» — лишь «не та, что
     * в DR0»: инкрементальная сборка может нести старый time_service.o с
     * датой многомесячной давности. Поэтому то же правило архива, что и при
     * потере календаря; только нечитаемый архив здесь не повод не верить. */
    if (sync == POLICY_TIME_SYNC_NEW_BUILD)
    {
        if (archive_read && last_ts[0] != '\0' && strcmp(last_ts, build_ts) >= 0)
            return 0U;
        return 1U;
    }

    /* Календарь потерян. Не знаем, что в архиве, — не верим. */
    if (!archive_read)
        return 0U;

    /* В архиве уже есть строки не раньше времени сборки — прибор жил после
     * прошивки, и часы откатились назад. */
    if (last_ts[0] != '\0' && strcmp(last_ts, build_ts) >= 0)
        return 0U;

    /* Архив пуст или кончается раньше сборки — так выглядит прошивка свежей
     * сборкой на обесточенной плате: время сборки — лучшее, что есть. */
    return 1U;
}
