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
