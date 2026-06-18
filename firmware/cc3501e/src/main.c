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
#include "worker.h"
#include "../hal/cc3501e_hw.h"

#if defined(CC3501E_RTOS_FREERTOS)
/* ----------------------------------------------------------------- *
 * ti bench build (FreeRTOS).  The bridge SPI slave runs WITHOUT host- *
 * DMA (the on-chip radio claims all 12 host-DMA channels): it opens   *
 * BLOCKING and is serviced by a dedicated blocking-poll TASK that     *
 * transport_spi_init() spawns (hal/ti/transport_hw_ti_spi.c).  That   *
 * poll task busy-waits the RX FIFO, so it is created ONE PRIORITY      *
 * BELOW this bring-up task -- which therefore must stay higher so it   *
 * can preempt the spinning poll task to drain the async worker (the   *
 * seconds-long Wlan_* lazy-start) and run the housekeeping tick.      *
 * (The native/stub build keeps the bare WFI loop below.)              *
 * ----------------------------------------------------------------- */
#include <FreeRTOS.h>
#include <task.h>

/* 2048 words = 8 KB.  Was 512 (2 KB), which OVERFLOWED on the Wi-Fi build: this
 * task runs cc3501e_hw_wifi_boot_start -> Wlan_Start -> InitHostDriver ->
 * init_device -> FW download, a deep CC35xx host-driver chain.  TI's
 * network_terminal runs Wlan_Start on a 6048-byte thread (main_freertos.c
 * THREADSTACKSIZE); 8 KB gives margin.  A 2 KB overflow faulted the task before
 * Wlan_Start returned, leaving the bridge SPI dead from boot (ping_ok=0,
 * reqhdr_rx=0x00000000) -- bench-proven.  (The transport + FW-event threads
 * Wlan_Start spawns get their own 4096+1200-byte stacks from the FreeRTOS heap.) */
#define CC3501E_BRINGUP_STACK_WORDS 2048u
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

	/* Confirm the MCUboot/PSA-FWU image FIRST (psa_fwu_accept, run by the first
	 * cc3501e_hw_tick) -- BEFORE anything that might block.  SHIP-CRITICAL
	 * ordering: a freshly-programmed vendor image boots as an unconfirmed TRIAL
	 * and is REVERTED by the cold BL2/MCUboot on the next boot unless the running
	 * app accepts it.  If a later step hangs (e.g. the radio bring-up) before the
	 * accept runs, the trial is never confirmed -> every subsequent cold boot
	 * reverts it -> the vendor image stops launching (host reads
	 * reqhdr_rx=0xFFFFFFFF) -- bench-proven 2026-06-18.  Accepting here, right
	 * after the bridge is armed, makes the image permanent regardless of what the
	 * radio does. */
	cc3501e_hw_tick();

#ifdef CC3501E_BLE_SELFTEST
	/* BLE BRING-UP SELFTEST (bench RF/antenna isolation, REVERT after) -- enable +
	 * advertise at BOOT, directly from this task: NO host poll, NO worker job, NO
	 * dependency on the inter-chip bridge.  If the unit then advertises "ALP-CC3501E"
	 * (visible to a BLE scanner / the bench PC), the CC35 NimBLE stack + RF front-end
	 * CAN bring BLE up -> the earlier worker/bridge-routed enable was the blocker.  If
	 * it stays silent, the block is NWP/RF (antenna), not our firmware context.  Runs
	 * once, then falls through to the normal loop. */
	(void)cc3501e_hw_ble_enable();
	(void)cc3501e_hw_ble_adv_start(1u /* connectable */, 100u, 100u, 0, 0u);
#endif

	/* Radio is brought up LAZILY on the first Wi-Fi op (from the worker drain),
	 * NOT at boot.  Wlan_Start can be slow/blocking (host-driver + NWP FW
	 * download); running it here would stall the bridge before any PING and, if it
	 * ever hangs, take the whole link down.  Deferring keeps the PING / IO /
	 * config link rock-solid and isolates the radio to the GET_MAC / Wi-Fi path
	 * (which re-opens the bridge SPI after the op -- see cc3501e_hw_ti.c).
	 * cc3501e_hw_wifi_boot_start() is left available but intentionally not called. */

	for (;;) {
		/* DRAIN the async worker OUTSIDE the SPI ISR: a QUEUED job (e.g.
		 * GET_MAC submitted from the ISR's poll) runs its blocking HAL body
		 * HERE, on the task, for as long as it needs (Wi-Fi init takes
		 * seconds).  Meanwhile the SPI ISR keeps answering the host's poll
		 * re-issues from the worker's shared state -- the whole point of the
		 * submit/poll seam (P0-4). */
		worker_run_pending();
		cc3501e_hw_tick();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

int main(void)
{
	cc3501e_hw_init();
	worker_init();
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
	worker_init();

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
		/* Drain any QUEUED async job (the submit/poll seam) on this idle
		 * wakeup, then run housekeeping.  On the silicon-free build the
		 * drain is a no-op (the stub runs jobs synchronously at submit). */
		worker_run_pending();
		cc3501e_hw_tick();
	}
	/* unreachable */
	return 0;
}

#endif /* CC3501E_RTOS_FREERTOS */
