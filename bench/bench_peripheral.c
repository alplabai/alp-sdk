/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/peripheral.h>.  v0.3 ships only the
 * rejection-cost cases (NULL config / bad bus_id) because the
 * Yocto + baremetal backends still NOSUPPORT-stub the real bus
 * paths.  v1.0 adds open/transfer/close round-trips per backend.
 */

#include <stddef.h>

#include "bench.h"

#include "alp/peripheral.h"

void bench_peripheral_main(void)
{
    /* alp_i2c_open(NULL) hits the cfg == NULL early-return path and
     * stamps alp_last_error before returning NULL.  Bench measures
     * the *rejection cost* -- the price a caller pays when probing
     * whether the peripheral is wired (e.g. a chip driver's optional
     * init guard).  Real-HW open/transfer benches arrive v1.0. */
    BENCH_RUN("alp_i2c_open(NULL)", 1000000, { (void)alp_i2c_open(NULL); });

    BENCH_RUN("alp_spi_open(NULL)", 1000000, { (void)alp_spi_open(NULL); });

    BENCH_RUN("alp_uart_open(NULL)", 1000000, { (void)alp_uart_open(NULL); });

    /* alp_gpio_open with a sentinel bad pin -- the wrapper rejects
     * via alp_last_error = OUT_OF_RANGE without touching a HW path. */
    BENCH_RUN("alp_gpio_open(0xFFFF)", 1000000, { (void)alp_gpio_open(0xFFFFu); });
}
