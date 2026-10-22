#include "power_policy.h"

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

uint8_t Policy_UsbKeepsAwake(Policy_Usb *u, uint8_t usb_present, uint8_t usb_configured,
                             uint8_t vbus_edge, uint32_t dt_ms, uint32_t enum_max_ms)
{
    /* Кабель вынули или вставили заново — прошлая неудача не в счёт. */
    if (!usb_present || vbus_edge)
        u->unconfigured_ms = 0;

    if (!usb_present)
        return 0U;

    if (usb_configured)
    {
        u->unconfigured_ms = 0;
        return 1U;
    }

    u->unconfigured_ms = (dt_ms > UINT32_MAX - u->unconfigured_ms)
                       ? UINT32_MAX : u->unconfigured_ms + dt_ms;
    return (u->unconfigured_ms < enum_max_ms) ? 1U : 0U;
}

uint8_t Policy_MeasureDue(uint32_t awake_ms, uint32_t interval_ms)
{
    return (awake_ms >= interval_ms) ? 1U : 0U;
}

uint32_t Policy_SleepChunkSec(uint32_t left_sec, uint32_t kick_sec)
{
    if (kick_sec == 0U || left_sec < kick_sec)
        return left_sec;
    return kick_sec;
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
