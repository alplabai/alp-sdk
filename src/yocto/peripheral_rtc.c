/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace RTC backend for <alp/rtc.h>.  Stub awaiting a real
 * `clock_gettime(CLOCK_REALTIME)` + `/dev/rtc0` implementation.
 * Every entry point stamps ALP_ERR_NOSUPPORT on the process-wide
 * last-error slot and returns the documented error sentinel so apps
 * detect-and-fallback via `alp_last_error()`.  Real backend tracked
 * in VERSIONS.md alongside the rest of the Yocto first-class
 * peripheral work.
 */

#if !defined(__linux__)
#error "peripheral_rtc.c (yocto backend) requires a Linux target"
#endif

#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/rtc.h"
#include "alp_internal.h"

alp_rtc_t *alp_rtc_open(uint32_t rtc_id)
{
    (void)rtc_id;
    alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_rtc_set_time(alp_rtc_t *rtc, const alp_rtc_time_t *time)
{
    (void)rtc;
    (void)time;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_rtc_get_time(alp_rtc_t *rtc, alp_rtc_time_t *time)
{
    (void)rtc;
    (void)time;
    return ALP_ERR_NOSUPPORT;
}

void alp_rtc_close(alp_rtc_t *rtc)
{
    (void)rtc;
}
