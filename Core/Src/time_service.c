#include "time_service.h"
#include <stdio.h>

/* Время сборки, разобранное из __DATE__ ("Mmm dd yyyy") и __TIME__ ("hh:mm:ss").
 * Это ближайшее к реальному время, доступное без внешнего источника: часы
 * выставляются в момент компиляции прошивки. Если между сборкой и прошивкой
 * прошло заметное время — на столько метки и будут отставать. */
#define BUILD_YEAR   ((__DATE__[7]  - '0') * 1000 + (__DATE__[8] - '0') * 100 + \
                      (__DATE__[9]  - '0') * 10   + (__DATE__[10] - '0'))
#define BUILD_MONTH  (__DATE__[0] == 'J' ? (__DATE__[1] == 'a' ? 1 : (__DATE__[2] == 'n' ? 6 : 7)) : \
                      __DATE__[0] == 'F' ? 2  : \
                      __DATE__[0] == 'M' ? (__DATE__[2] == 'r' ? 3 : 5) : \
                      __DATE__[0] == 'A' ? (__DATE__[1] == 'p' ? 4 : 8) : \
                      __DATE__[0] == 'S' ? 9  : \
                      __DATE__[0] == 'O' ? 10 : \
                      __DATE__[0] == 'N' ? 11 : 12)
#define BUILD_DAY    ((__DATE__[4] == ' ' ? 0 : __DATE__[4] - '0') * 10 + (__DATE__[5] - '0'))
#define BUILD_HOUR   ((__TIME__[0] - '0') * 10 + (__TIME__[1] - '0'))
#define BUILD_MIN    ((__TIME__[3] - '0') * 10 + (__TIME__[4] - '0'))
#define BUILD_SEC    ((__TIME__[6] - '0') * 10 + (__TIME__[7] - '0'))

/* Отпечаток сборки в резервном регистре RTC. Календарь живёт в backup-домене и
 * переживает сброс, поэтому по одному лишь «календарь не инициализирован» новую
 * прошивку не отличить от обычного ресета. Сравнение отпечатка даёт то, что нужно:
 * перепрошил — часы переставились, просто сбросил — время сохранилось. */
#define BUILD_STAMP  ((uint32_t)(BUILD_YEAR * 10000UL + BUILD_MONTH * 100UL + BUILD_DAY) ^ \
                      (uint32_t)(BUILD_HOUR * 10000UL + BUILD_MIN * 100UL + BUILD_SEC) << 8)

void Time_InitOnce(void)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    sTime.Hours   = BUILD_HOUR;
    sTime.Minutes = BUILD_MIN;
    sTime.Seconds = BUILD_SEC;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    sDate.Month = BUILD_MONTH;
    sDate.Date  = BUILD_DAY;
    sDate.Year  = BUILD_YEAR - 2000;
    /* День недели RTC сам не вычисляет, а для меток времени он не используется —
     * ставим фиксированное валидное значение, чтобы пройти проверку параметров. */
    sDate.WeekDay = RTC_WEEKDAY_MONDAY;

    HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
}

uint8_t Time_SyncToBuildIfNeeded(void)
{
    HAL_PWR_EnableBkUpAccess();

    uint8_t calendar_lost = (__HAL_RTC_IS_CALENDAR_INITIALIZED(&hrtc) == 0U);
    uint8_t new_build     = (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != BUILD_STAMP);

    if (!calendar_lost && !new_build)
        return 0;   /* та же прошивка и часы идут — не трогаем */

    Time_InitOnce();
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, BUILD_STAMP);
    return 1;       /* часы переставлены на время сборки */
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
