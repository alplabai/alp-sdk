/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for the diagnostic surface (alp_last_error +
 * status-code translation).  These paths sit on the inner loop of
 * every wrapper open/transfer call, so even a few nanoseconds
 * matter.
 */

#include "bench.h"

#include "alp/peripheral.h"

void bench_status_main(void)
{
    /* alp_last_error is a thread-local read on Zephyr (TLS) and a
     * single static read on baremetal / yocto.  Either way it
     * should be one or two instructions plus the call overhead. */
    BENCH_RUN("alp_last_error", 1000000, { (void)alp_last_error(); });
}
