#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "../main/report_calendar.h"

static void check_cutoff(int hour, int min, bool expected) {
    struct tm t = {0};
    t.tm_hour = hour;
    t.tm_min = min;
    assert(report_after_daily_cutoff(&t) == expected);
}

int main(void) {
    assert(setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1) == 0);
    tzset();

    assert(report_shift_day(20261112u, -1) == 20261111u);
    assert(report_shift_day(20261111u, 1) == 20261112u);
    assert(report_shift_day(20260101u, -1) == 20251231u);
    assert(report_shift_day(20261231u, 1) == 20270101u);
    assert(report_shift_day(20260228u, 1) == 20260301u);
    assert(report_shift_day(20240228u, 1) == 20240229u);
    assert(report_shift_day(20240229u, 1) == 20240301u);
    assert(report_shift_day(20260328u, 1) == 20260329u);
    assert(report_shift_day(20260329u, 1) == 20260330u);
    assert(report_shift_day(20261024u, 1) == 20261025u);
    assert(report_shift_day(20261025u, 1) == 20261026u);
    assert(report_shift_day(20260230u, 1) == 0);
    assert(report_shift_day(0, 1) == 0);
    check_cutoff(0, 0, false);
    check_cutoff(0, 1, false);
    check_cutoff(0, 2, true);
    check_cutoff(0, 59, true);
    check_cutoff(1, 0, true);
    check_cutoff(23, 59, true);
    // The persisted last completed day drives gap-free recovery after downtime.
    // Example: device last reported Sun 08.11, returns Mon 16.11 after 00:02:
    // must send Nov 9, 10, ... 15 (incl. a complete Sunday/week report).
    uint32_t checkpoint = 20261108u;
    uint32_t eligible_through = 20261115u;
    uint32_t expected = 20261109u;
    int recovered = 0;
    while (checkpoint < eligible_through) {
        uint32_t requested = report_shift_day(checkpoint, 1);
        assert(requested == expected);
        checkpoint = requested; // only after HTTP confirmation / NVS checkpoint
        expected = report_shift_day(expected, 1);
        recovered++;
    }
    assert(recovered == 7 && checkpoint == eligible_through);
    puts("PASS report calendar: cutoffs, year boundaries, leap years, Kyiv DST");
    return 0;
}
