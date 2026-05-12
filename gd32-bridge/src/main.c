/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware -- entry point.
 *
 * The GD32G553 supervisor runs this firmware as its sole user
 * application (no RTOS).  The runtime model is purely
 * interrupt-driven: SPI slave + I2C slave each have their own ISR
 * that hands complete request envelopes off to protocol_dispatch()
 * (in protocol.c) and stages the matching reply envelope back to
 * the transport.
 *
 * The main loop has no work of its own; it sleeps in WFI() so the
 * Cortex-M33 idles between interrupts.  A periodic SysTick polls
 * the DA9292 PMIC's PMC_STATUS_00 over I2C-master so the cached
 * value served by CMD_DA9292_STATUS_FORWARD stays fresh.
 *
 * Scaffold today: the bridge_hw_* HAL is stubbed; only PING,
 * GET_VERSION, GET_BUILD_ID, and RESET_REASON round-trip without
 * NOSUPPORT.  The HAL implementation against the GigaDevice
 * firmware library lands in a follow-up.
 */

#include <stdint.h>

#include "protocol.h"

/* Forward declarations of the per-transport init helpers (no public
 * headers for the transports -- main.c is the only caller). */
void transport_spi_init(void);
void transport_i2c_init(void);

/* Optional weak hooks the HAL layer can override.  Defaults to a
 * busy WFI loop -- behaviour-equivalent to a no-op for the
 * scaffold; the real HAL replaces this with a SysTick callback
 * that re-polls the PMIC. */
__attribute__((weak)) void bridge_hw_init(void) { }
__attribute__((weak)) void bridge_hw_tick(void) { }

/* The Cortex-M intrinsic; weakly defined here so the scaffold
 * compiles under hosted toolchains where __WFI() is missing. */
#ifndef __WFI
__attribute__((weak)) void __WFI(void) { }
#endif

int main(void)
{
    bridge_hw_init();
    transport_spi_init();
    transport_i2c_init();

    /* The SysTick / TIM-based periodic refresh of the cached DA9292
     * status lives inside bridge_hw_tick(); main loop just yields. */
    for (;;) {
        __WFI();
        bridge_hw_tick();
    }
    /* unreachable */
    return 0;
}
