#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include "rtc.h"
#include <stdint.h>

void Time_InitOnce(void);
void Time_GetTimestamp(char *buf, uint16_t len);

/* Сверяет отпечаток прошивки в резервном регистре RTC и, если он не совпал
 * (значит залита новая сборка) или календарь потерян, переставляет часы на
 * время компиляции. Возвращает POLICY_TIME_SYNC_* (power_policy.h): часы не
 * трогали, переставлены под новую сборку при живом календаре или календарь
 * был потерян (питание backup-домена пропадало). */
uint8_t Time_SyncToBuildIfNeeded(void);

/* Время сборки в формате меток CSV ("YYYY-MM-DD hh:mm:ss"). */
void Time_BuildTimestamp(char *buf, uint16_t len);

/* Достоверность часов (A6) — хранится в backup-регистре и переживает
 * сбросы вместе с календарём. Get возвращает 0, если отметки нет. */
uint8_t Time_GetTrusted(void);
void    Time_SetTrusted(uint8_t trusted);

#endif
