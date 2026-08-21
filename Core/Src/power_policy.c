#include "power_policy.h"

uint8_t Policy_ShouldWipeArchive(uint8_t key_held, uint8_t soft_reset)
{
    return (key_held && !soft_reset) ? 1U : 0U;
}

uint8_t Policy_MayEnterSleep(uint8_t key_held, uint8_t usb_present,
                             uint32_t held_ms, uint32_t hold_max_ms)
{
    /* Пока есть VBUS — не спим ни при каких условиях: потолок сюда не применяется. */
    if (usb_present)
        return 0U;

    if (!key_held)
        return 1U;

    return (held_ms >= hold_max_ms) ? 1U : 0U;
}
