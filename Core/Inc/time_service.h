#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include "rtc.h"
#include <stdint.h>

void Time_InitOnce(void);
void Time_GetTimestamp(char *buf, uint16_t len);

#endif
