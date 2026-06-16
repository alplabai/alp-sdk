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
 * Cortex-M33 idles between interrupts.  bridge_hw_tick() runs on the
 * wakeups as a periodic housekeeping hook; it is a no-op on this SoM
 * revision -- the DA9292 fault nets (DA9292_INT/DA9292_TW) reach only
 * the Renesas (P37/P36), the GD32 has no pin to sample and no I2C
 * path to the PMIC, so CMD_DA9292_STATUS_FORWARD serves the 0xFF
 * "no sample" sentinel.  Register-level PMIC status (PMC_STATUS_00
 * etc.) is read by the host over BRD_I2C via the chips/da9292 driver.
 *
 * Backends: BRIDGE_HAL_BACKEND=gd32 drives real silicon (peripheral
 * HAL in the per-peripheral TUs under hal/gd32/, SPI1 + I2C0 slave
 * transports in hal/transport_hw_gd32.c).  BRIDGE_HAL_BACKEND=stub keeps everything
 * hardware-free for host-side protocol tests: PING / GET_VERSION /
 * GET_BUILD_ID / RESET_REASON round-trip and HW-touching ops return
 * NOSUPPORT.
 */

#include <stdint.h>

#include "protocol.h"
#include "transport.h"

/* Optional weak hooks the HAL layer can override.  Defaults to a
 * busy WFI loop -- behaviour-equivalent to a no-op for the
 * scaffold; the real HAL overrides these for peripheral bring-up
 * and periodic housekeeping. */
__attribute__((weak)) void bridge_hw_init(void)
{
}
__attribute__((weak)) void bridge_hw_tick(void)
{
}

/* The Cortex-M intrinsic; weakly defined here so the scaffold
 * compiles under hosted toolchains where __WFI() is missing. */
#ifndef __WFI
__attribute__((weak)) void __WFI(void)
{
}
#endif

int main(void)
{
	bridge_hw_init();
	transport_spi_init();
	transport_i2c_init();

	/* Periodic housekeeping lives inside bridge_hw_tick() (a no-op on
     * this SoM rev); the main loop just yields. */
	for (;;) {
		__WFI();
		bridge_hw_tick();
	}
	/* unreachable */
	return 0;
}
