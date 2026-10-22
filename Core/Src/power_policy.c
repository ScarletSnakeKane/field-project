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
