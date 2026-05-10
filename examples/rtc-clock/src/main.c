/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rtc-clock — set the RTC, sleep, read it back.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/rtc.h"

int main(void) {
    printf("[rtc] open instance 0\n");
    alp_rtc_t *rtc = alp_rtc_open(0);
    if (rtc == NULL) {
        printf("[rtc] open failed: alp_last_error=%d "
               "(expected NOT_READY = -2 on native_sim)\n",
               (int)alp_last_error());
        printf("[rtc] done\n");
        return 0;
    }

    alp_rtc_time_t set_t = {
        .year = 2026, .month = 5, .day = 10,
        .hour = 14,   .minute = 30, .second = 0,
    };
    alp_status_t s = alp_rtc_set_time(rtc, &set_t);
    printf("[rtc] set_time(2026-05-10 14:30:00) -> %d\n", (int)s);

    k_msleep(250);

    alp_rtc_time_t got;
    s = alp_rtc_get_time(rtc, &got);
    printf("[rtc] get_time -> status=%d, %04u-%02u-%02u %02u:%02u:%02u\n",
           (int)s, got.year, got.month, got.day,
           got.hour, got.minute, got.second);

    alp_rtc_close(rtc);
    printf("[rtc] done\n");
    return 0;
}
