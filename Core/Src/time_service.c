#include "time_service.h"
#include <stdio.h>

void Time_InitOnce(void)
{
    // Пример: один раз выставить время/дату вручную
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    sTime.Hours   = 0x12;
    sTime.Minutes = 0x03;
    sTime.Seconds = 0x00;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    sDate.WeekDay = RTC_WEEKDAY_FRIDAY;
    sDate.Month   = RTC_MONTH_AUGUST;
    sDate.Date    = 0x01;  // 1
    sDate.Year    = 26;  // 2026

    HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
}

void Time_GetTimestamp(char *buf, uint16_t len)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

    snprintf(buf, len,
             "%04d-%02d-%02d %02d:%02d:%02d",
             2000 + d.Year, d.Month, d.Date,
             t.Hours, t.Minutes, t.Seconds);
}
