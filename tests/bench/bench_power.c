/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/power.h>.  Measures the open + wake-
 * configure + request_sleep dispatcher overhead on the stub-
 * backend path -- the price a caller pays per sleep-attempt
 * decision regardless of whether the backend actually sleeps.
 *
 * Real-silicon transition latency (RUN -> DEEP_SLEEP -> wake)
 * lives in the per-(SoM, OS) baselines and is dominated by the
 * vendor PMU's wake-up time, not the SDK dispatcher.
 */

#include <stddef.h>

#include "bench.h"

#include "alp/power.h"

void bench_power_main(void)
{
    BENCH_RUN("alp_power_open", 1000000,
              { alp_power_close(alp_power_open()); });

    /* The realistic per-iteration shape: open, declare wake source,
     * close.  Captures the dispatcher cost a typical guarded sleep
     * path pays. */
    BENCH_RUN("alp_power_configure_wake_source(RTC)", 1000000, {
        alp_power_t *p = alp_power_open();
        (void)alp_power_configure_wake_source(p, ALP_POWER_WAKE_RTC);
        alp_power_close(p);
    });

    /* request_sleep with no wake configured + zero wake_after_ms --
     * the INVAL early-exit cost; representative of apps that try
     * to enter sleep without first arming a wake source. */
    BENCH_RUN("alp_power_request_sleep(INVAL no wake)", 1000000, {
        alp_power_t *p = alp_power_open();
        (void)alp_power_configure_wake_source(p, ALP_POWER_WAKE_NONE);
        (void)alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP,
                                      0u, NULL);
        alp_power_close(p);
    });

    BENCH_RUN("alp_power_close(NULL)", 1000000, { alp_power_close(NULL); });
}
