#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include "rtc.h"
#include <stdint.h>

void Time_InitOnce(void);
void Time_GetTimestamp(char *buf, uint16_t len);

/* Сверяет отпечаток прошивки в резервном регистре RTC и, если он не совпал
 * (значит залита новая сборка), переставляет часы на время компиляции.
 * Возвращает 1, если часы были переставлены. */
uint8_t Time_SyncToBuildIfNeeded(void);

#endif
