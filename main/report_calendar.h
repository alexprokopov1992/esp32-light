#ifndef POWER_REPORT_CALENDAR_H
#define POWER_REPORT_CALENDAR_H

// Pure C calendar helpers shared with host-side tests. The process timezone
// must already be set to Europe/Kyiv (set_tz_kyiv() in app_main.c).
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

static uint32_t local_yyyymmdd(const struct tm *tm_local) {
    if (!tm_local) return 0;
    return (uint32_t)(tm_local->tm_year + 1900) * 10000u +
           (uint32_t)(tm_local->tm_mon + 1) * 100u +
           (uint32_t)tm_local->tm_mday;
}

// Use local NOON for calendar-day increments; subtracting 86400 seconds from
// midnight is wrong on Kyiv's 23/25-hour daylight-saving transition days.
static uint32_t report_shift_day(uint32_t day, int shift) {
    if (day < 20240101u || day > 21001231u) return 0;
    struct tm local = {0};
    local.tm_year = (int)(day / 10000u) - 1900;
    local.tm_mon = (int)((day / 100u) % 100u) - 1;
    local.tm_mday = (int)(day % 100u);
    local.tm_hour = 12;
    local.tm_isdst = -1;
    time_t t = mktime(&local);
    if (t == (time_t)-1) return 0;
    struct tm check;
    localtime_r(&t, &check);
    if (local_yyyymmdd(&check) != day) return 0; // invalid NVS date
    local.tm_mday += shift;
    local.tm_isdst = -1;
    t = mktime(&local);
    if (t == (time_t)-1) return 0;
    localtime_r(&t, &check);
    return local_yyyymmdd(&check);
}

static bool report_after_daily_cutoff(const struct tm *local) {
    return local && (local->tm_hour > 0 || local->tm_min >= 2);
}

#endif // POWER_REPORT_CALENDAR_H
