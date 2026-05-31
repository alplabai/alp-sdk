/*
 * Copyright 2026 Alp Lab AB
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
 * Backends: BRIDGE_HAL_BACKEND=gd32 drives real silicon (peripheral
 * HAL in hal/bridge_hw_gd32.c, SPI1 + I2C0 slave transports in
 * hal/transport_hw_gd32.c).  BRIDGE_HAL_BACKEND=stub keeps everything
 * hardware-free for host-side protocol tests: PING / GET_VERSION /
 * GET_BUILD_ID / RESET_REASON round-trip and HW-touching ops return
 * NOSUPPORT.
 */

#include <stdint.h>

#include "protocol.h"
#include "transport.h"

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
