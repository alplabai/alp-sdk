/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware -- entry point.
 *
 * The CC3501E's Cortex-M application core runs this firmware as the
 * SPI-slave (or, on SDIO-routed boards, SDIO-slave) parser that fronts
 * TI's SimpleLink Wi-Fi + BLE stacks for the Alif host.  The runtime
 * model is interrupt-driven: the active transport's slave peripheral
 * ISR hands complete request frames to protocol_dispatch() (via
 * protocol_build_reply) and stages the reply; the main loop just idles
 * in WFI between interrupts (the gd32-bridge model).
 *
 * Selectable host-control transport (CC3501E_CONTROL_TRANSPORT, default
 * spi).  SPI is the always-available baseline/fallback; SDIO is opt-in
 * for boards that dedicate the Alif's single SDIO controller to the
 * CC3501E (no SD card).  Exactly one is active.  See transport.h +
 * docs/cc3501e-bridge.md.
 *
 * Backends: CC3501E_HAL_BACKEND=stub keeps everything hardware-free for
 * host-side protocol tests (PING / GET_VERSION round-trip; HW-touching
 * ops report NOTIMPL).  CC3501E_HAL_BACKEND=ti drives real silicon
 * against TI's SimpleLink CC33xx SDK (built on the bench).
 */

#include "transport.h"
#include "../hal/cc3501e_hw.h"

#if defined(CC3501E_RTOS_FREERTOS)
/* ----------------------------------------------------------------- *
 * ti bench build (FreeRTOS).  The CC35xx TI Drivers dpl is FreeRTOS-  *
 * backed -- the SPI/SDIO slave's transfer-complete callback dispatches *
 * via SwiP, which only runs once the scheduler is started.  So main() *
 * starts a single bring-up task that arms the active transport and    *
 * runs the housekeeping tick; the slave callback then fires from the  *
 * driver's ISR/SwiP.  (The native/stub build keeps the bare WFI loop  *
 * below -- the gd32-bridge model.)                                    *
 * ----------------------------------------------------------------- */
#include <FreeRTOS.h>
#include <task.h>

#define CC3501E_BRINGUP_STACK_WORDS 512u
static StackType_t  bringup_stack[CC3501E_BRINGUP_STACK_WORDS];
static StaticTask_t bringup_tcb;

static void bringup_task(void *arg)
{
	(void)arg;
#if defined(CC3501E_CONTROL_TRANSPORT_SDIO)
	transport_sdio_init();
#else
	transport_spi_init();
#endif
	for (;;) {
		cc3501e_hw_tick();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

int main(void)
{
	cc3501e_hw_init();
	(void)xTaskCreateStatic(bringup_task, "cc3501e_bringup", CC3501E_BRINGUP_STACK_WORDS, NULL,
	                        configMAX_PRIORITIES - 1, bringup_stack, &bringup_tcb);
	vTaskStartScheduler();
	for (;;) {
	}
	/* unreachable */
}

#else /* bare-loop: native/stub build (gd32-bridge model) */

/* The Cortex-M intrinsic; weakly defined here so the scaffold compiles
 * under hosted toolchains where __WFI() is missing. */
#ifndef __WFI
__attribute__((weak)) void __WFI(void)
{
}
#endif

int main(void)
{
	cc3501e_hw_init();

#if defined(CC3501E_CONTROL_TRANSPORT_SDIO)
	/* Opt-in: the board routes the Alif SDIO controller to the CC3501E
     * (no SD card present).  Falls back to SPI by simply not defining
     * this at build time. */
	transport_sdio_init();
#else
	transport_spi_init(); /* default + always-available baseline */
#endif

	for (;;) {
		__WFI();
		cc3501e_hw_tick();
	}
	/* unreachable */
	return 0;
}

#endif /* CC3501E_RTOS_FREERTOS */
