/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace watchdog backend for <alp/wdt.h>.  Stub awaiting a
 * real `/dev/watchdogN` ioctl implementation.  Every entry point
 * stamps ALP_ERR_NOSUPPORT on the process-wide last-error slot and
 * returns the documented error sentinel so apps detect-and-fallback
 * via `alp_last_error()`.  Real backend tracked in VERSIONS.md
 * alongside the rest of the Yocto first-class peripheral work.
 */

#if !defined(__linux__)
#error "peripheral_wdt.c (yocto backend) requires a Linux target"
#endif

#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/wdt.h"
#include "alp_internal.h"

alp_wdt_t *alp_wdt_open(uint32_t wdt_id, const alp_wdt_config_t *cfg)
{
    (void)wdt_id;
    (void)cfg;
    alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_wdt_feed(alp_wdt_t *wdt)
{
    (void)wdt;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_wdt_disable(alp_wdt_t *wdt)
{
    (void)wdt;
    return ALP_ERR_NOSUPPORT;
}

void alp_wdt_close(alp_wdt_t *wdt)
{
    (void)wdt;
}
