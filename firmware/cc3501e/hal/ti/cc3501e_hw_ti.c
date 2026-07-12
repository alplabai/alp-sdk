/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- platform lifecycle (chip init, the idle
 * housekeeping tick, and the deferred self-reset / OTA swap-reboot latch).
 *
 * Split by hardware subsystem out of this file (issue #703, #461 Phase B):
 * cc3501e_hw_ti_wifi.c (Wi-Fi + factory MAC), cc3501e_hw_ti_ble.c (BLE),
 * cc3501e_hw_ti_sock.c (TCP/UDP sockets), cc3501e_hw_ti_gpio.c (GPIO proxy +
 * camera enables + the bridge READY line), cc3501e_hw_ti_power.c (power
 * policy), cc3501e_hw_ti_ota.c (host-driven OTA-over-bridge streaming), and
 * cc3501e_hw_ti_log.c (log level + diagnostics).  This file keeps chip
 * bring-up, the idle tick, the boot-time TRIAL-image accept + SELFTEST
 * installer (both run ONLY from cc3501e_hw_tick, not from a host OTA
 * session), and the deferred-reboot latch (reply_drained /
 * ota_reboot_pending / ota_reboot_rc) that cc3501e_hw_tick() sequences on
 * behalf of both CMD_RESET and cc3501e_hw_ti_ota.c's FINISH/PROMOTE bodies
 * -- see cc3501e_hw_ti_internal.h for that cross-TU seam.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK (the CC3501E is a SimpleLink Wi-Fi 6 + BLE 5.4
 * wireless MCU; we run this firmware on its application core).  CI builds
 * the stub backend instead, so this file is never on the SDK-free path.
 *
 * API grounding: CMSIS NVIC_SystemReset() for the MCU reset; the
 * board-specific anchors (CONFIG_SPI_0, pin mux) come from the SDK's
 * SysConfig output (ti_drivers_config.h) generated for the E1M-AEN board
 * file -- resolved at bench-build time, not invented here.
 */

#include <stdbool.h>
#include <stdint.h>

/* CMSIS core for NVIC_SystemReset (the CC35xx M33 core) + the PRIMASK
 * intrinsics (worker_critical_enter/exit).  ti_drivers_config.h does NOT
 * bring the core in transitively on this SDK (SDK 10.10). */
#include <ti/devices/cc35xx/cmsis/device.h>

#include "ti_drivers_config.h"

#include <ti/drivers/SPI.h>
#include <ti/utils/FWU/psa_fwu.h> /* PSA Firmware Update: accept the MCUboot TRIAL image (cold-boot fix) */

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* reply_drained / ota_reboot_pending / ota_reboot_rc / cc3501e_hw_ota_pump */
#include "transport.h" /* bridge_transport_spi_hw_reinit (Wlan_Start DMA-coexistence fix) */

/* Bridge SPI desync counter (transport_hw_ti_spi.c): increments each time the
 * slave re-arms the header phase on a reserved-range/0xA5 header (a misframe).
 * A burst in one housekeeping tick means the link is stuck desynced + the RX
 * FIFO likely holds residue -- cc3501e_hw_tick() flushes via a full SPI re-open. */
extern volatile uint32_t g_resync_count;

/* Deferred-reset latch: CMD_RESET sets this; cc3501e_hw_tick() performs
 * the reboot on the next idle wakeup, but ONLY after reply_drained
 * confirms the OK ack has fully clocked back to the host. */
static volatile bool reset_pending;

/* Set by cc3501e_hw_notify_reply_sent() when the in-flight reply (for
 * CMD_RESET, the ack itself) has fully clocked out; cleared by
 * cc3501e_hw_request_reset() so only that command's own ack arms the
 * reboot.  Gates reset_pending in cc3501e_hw_tick(). */
volatile bool reply_drained; /* shared with cc3501e_hw_ti_ota.c -- see cc3501e_hw_ti_internal.h */

/* Deferred OTA swap-reboot latch: cc3501e_hw_ota_finish() sets it; cc3501e_hw_tick()
 * calls psa_fwu_request_reboot() once reply_drained confirms the FINISH ack has
 * clocked back (same ack-before-reboot race fix as reset_pending). */
volatile bool
    ota_reboot_pending; /* shared with cc3501e_hw_ti_ota.c -- see cc3501e_hw_ti_internal.h */
/* Result of the last psa_fwu_request_reboot() -- request_reboot only RETURNS if
 * the swap was refused (BL2 anti-rollback on a downgrade, no pending image, …);
 * on success it reboots and never returns.  0 = none requested yet.  Surfaced in
 * the OTA_STATUS reserved byte so the host can tell "refused" from "never fired". */
volatile int8_t ota_reboot_rc; /* shared with cc3501e_hw_ti_ota.c -- see cc3501e_hw_ti_internal.h */

/* Minimal OSI sleep shim for the PSA-FWU lib (FWU.a references osi_uSleep -- the Wi-Fi host
 * driver's OSI microsecond sleep -- for a flash-commit settle).  The full OSI layer lives in
 * the Wi-Fi platform we do NOT link in v0.1, and psa_fwu_accept() runs PRE-scheduler (from
 * cc3501e_hw_init, before vTaskStartScheduler) where a yielding sleep can't run, so use a
 * busy-wait.  Signature matches <ti/drivers/net/wifi/wifi_host_driver/inc_adapt/osi_kernel.h>
 * (OsiReturnVal_e osi_uSleep(OsiTime_t); OSI_OK == 0).  Over-delay is harmless.
 * Compiled out under CC3501E_WIFI: the real OSI in wifi_platform_cc35xx.a then provides
 * osi_uSleep, and defining it here too would be a multiple-definition at link (P0-5). */
#ifndef CC3501E_WIFI
int osi_uSleep(unsigned int usec)
{
	volatile unsigned int spins = usec * 100u; /* generous busy-wait; commit settle only */
	while (spins--) {
		__asm__ volatile("nop");
	}
	return 0; /* OSI_OK */
}
#endif /* !CC3501E_WIFI */

/* ---- async-worker ISR/drain mutual exclusion (P0-4) -------------- *
 * The silicon-free worker (src/worker.c) publishes a job result with a
 * multi-field write that the SPI ISR (which polls that result) must never
 * observe half-done.  worker.c declares these as WEAK no-ops (correct for
 * the single-threaded native build); on real silicon the SPI driver's
 * transfer callback runs in HWI/SwiP context, so we mask interrupts for
 * the short publish/read window.  PRIMASK save/restore (not a bare
 * enable) so a nested critical section can't prematurely re-enable. */
unsigned long worker_critical_enter(void)
{
	const unsigned long key = __get_PRIMASK();
	__disable_irq();
	return key;
}

void worker_critical_exit(unsigned long key)
{
	__set_PRIMASK(key);
}

void cc3501e_hw_init(void)
{
	/* TI Drivers core init.  GPIO + SPI are brought up here so the
	 * transport's hw-init hook can open its handles.  No network
	 * processor in v0.1 (see file header). */
	/* The SPI-slave driver is SPIWFF3DMA: it is DMA-based and its SPI_open() calls
	 * Power_setDependency(), so the Power AND DMA drivers MUST be initialized before
	 * any driver open.  Board_init() (generated ti_drivers_config.c) =
	 * Power_init() + GPIO_init() + DMAWFF3_init(); every CC35X1 SDK example calls it
	 * first.  Calling only GPIO_init()+SPI_init() (the prior bug) left Power/DMA
	 * uninitialized -> SPI_open faulted -> the slave never armed and POCI was never
	 * driven (root-caused 2026-06-17 via deep analysis). */
	Board_init();
	SPI_init();
	/* The MCUboot/PSA-FWU TRIAL-image accept is done on the first cc3501e_hw_tick()
	 * (POST-scheduler), NOT here -- the HSM/FWU services that finalize the OTA commit
	 * only run once the FreeRTOS scheduler is up.  See cc3501e_hw_tick(). */
}

#ifdef CC3501E_OTA_SELFTEST
/* ===================================================================== */
/* OTA update cycle (PSA-FWU) -- the supported path to install a CONFIRMED,
 * cold-launchable PRIMARY vendor image on an ALREADY-ACTIVATED unit.
 *
 * A directly-`programming`-written primary is NOT cold-launchable (proven on
 * silicon 2026-06-17); only `factory_programming`'s atomic activation-context
 * write (fresh unit) or an OTA SWAP produces a cold-launchable primary.  This
 * runs the real OTA cycle: write a candidate to the non-primary vendor slot ->
 * install -> reboot, so the cold BL2/MCUboot SWAPS secondary->primary and boots
 * it as a TRIAL; the swapped image's first boot then psa_fwu_accept()s it
 * (cc3501e_hw_tick below, non-SELFTEST build) -> permanent + cold-launchable.
 *
 * SELFTEST drives it from an EMBEDDED candidate (the plain v0.0.4.0 signed
 * vendor image) to validate the mechanism without a host transport; the SAME
 * cc3501e_ota_install() will be driven by OTA-over-bridge commands as the v0.2
 * OTA feature (Mender contract).  The candidate is the GPE-format signed vendor
 * image: first TI_FWU_MANIFEST_SIZE bytes are the manifest (psa_fwu_start), the
 * rest is streamed via psa_fwu_write (mirrors examples/.../ota_example). */
extern const unsigned char cc3501e_ota_candidate[];
extern const unsigned int  cc3501e_ota_candidate_len;

/* psa_fwu_write granularity for the SELFTEST installer: 256 B = the CC35 flash
 * page (a 4-byte-aligned flash write block).  This is the validated selftest
 * chunk (host cc3501e_ota_update mirrors it: chips/cc3501e/cc3501e.c documents
 * "the validated SELFTEST installer used CC3501E_OTA_WRITE_CHUNK 256").  Its
 * original definition here was dropped when the OTA-over-bridge FINISH burst
 * (CC3501E_OTA_FINISH_FLASH_BLOCK, below) was added, which broke --ota-selftest
 * builds -- restored so the selftest installer's use (below) resolves. */
#define CC3501E_OTA_WRITE_CHUNK 256u

/* Install a GPE-format signed vendor image into the alternate (non-primary)
 * vendor slot and request the reboot that performs the swap.  Returns 0 on
 * "install staged + reboot requested" (does not return if reboot is immediate),
 * negative on any failure (caller falls through harmlessly). */
static int cc3501e_ota_install(const unsigned char *img, unsigned int len)
{
	psa_fwu_component_info_t i1, i2, ti;
	psa_fwu_component_t      target;
	unsigned int             off;

	if (img == 0 || len <= TI_FWU_MANIFEST_SIZE) {
		return -1;
	}
	/* Pick the non-primary vendor slot as the update target (slot 4 <-> 5). */
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_1, &i1) != PSA_SUCCESS) {
		return -2;
	}
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_2, &i2) != PSA_SUCCESS) {
		return -2;
	}
	if (i1.impl.Primary && !i2.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_2;
	} else if (i2.impl.Primary && !i1.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_1;
	} else {
		return -3; /* can't identify a single non-primary target */
	}

	/* Bring the target slot to READY (clean/cancel a stale candidate). */
	if (psa_fwu_query(target, &ti) != PSA_SUCCESS) {
		return -4;
	}
	if (ti.state != PSA_FWU_READY) {
		if (ti.state == PSA_FWU_WRITING || ti.state == PSA_FWU_CANDIDATE) {
			psa_fwu_cancel(target);
		} else {
			psa_fwu_clean(target);
		}
	}

	/* Manifest = first TI_FWU_MANIFEST_SIZE bytes; image = the remainder. */
	if (psa_fwu_start(target, img, TI_FWU_MANIFEST_SIZE) != PSA_SUCCESS) {
		return -5;
	}
	for (off = TI_FWU_MANIFEST_SIZE; off < len;) {
		unsigned int n = len - off;
		if (n > CC3501E_OTA_WRITE_CHUNK) {
			n = CC3501E_OTA_WRITE_CHUNK;
		}
		if (psa_fwu_write(target, off, img + off, n) != PSA_SUCCESS) {
			return -6;
		}
		off += n;
	}
	if (psa_fwu_finish(target) != PSA_SUCCESS) {
		return -7;
	}
	if (psa_fwu_install() != PSA_SUCCESS) { /* CANDIDATE -> STAGED */
		return -8;
	}
	psa_fwu_request_reboot(); /* reboot -> BL2 swaps secondary->primary -> TRIAL boot */
	return 0;
}
#endif /* CC3501E_OTA_SELFTEST */

void cc3501e_hw_tick(void)
{
	/* === COLD-BOOT FIX (2026-06-17): accept the MCUboot / PSA-FWU TRIAL image (one-shot). ===
	 * TRM SWRU626 §10.3.2: CC35xx vendor images are MCUboot images.  On this OTA device a
	 * freshly-programmed vendor image boots in PSA_FWU_TRIAL and is REVERTED by the cold
	 * BL2/MCUboot Chain-of-Trust on the next boot UNLESS the RUNNING app accepts (commits) it.
	 * A warm/programmer/debug launch bypasses the trailer check and runs the trial (so PING
	 * worked), but it was never confirmed -> every cold power-on reverted the unconfirmed
	 * trial and the vendor image never launched (host saw reqhdr_rx=0xFFFFFFFF).  Accept on
	 * the FIRST tick -- this runs POST-scheduler (from bringup_task), where the HSM/FWU
	 * services that finalize the commit are up (accept from cc3501e_hw_init, pre-scheduler,
	 * does not finalize).  Mirrors examples/.../ota_example: init -> accept -> reboot-to-commit.
	 * After commit the image is permanent -> subsequent COLD boots launch it. */
	static bool fwu_accept_done = false;
	if (!fwu_accept_done) {
		fwu_accept_done = true;
		psa_fwu_init();
#ifdef CC3501E_OTA_SELFTEST
		/* One-shot bench validation: OTA-install the embedded candidate to the
		 * alternate vendor slot and reboot so BL2 swaps it to primary.  If it
		 * succeeds the device reboots here (does not return); the swapped image
		 * is a PLAIN build (no SELFTEST) that accept()s itself below on its
		 * TRIAL boot -> permanent.  If install fails, fall through to accept. */
		(void)cc3501e_ota_install(cc3501e_ota_candidate, cc3501e_ota_candidate_len);
#endif
		if (psa_fwu_accept() == PSA_SUCCESS_REBOOT) {
			psa_fwu_request_reboot(); /* finalize commit; device reboots, image now permanent */
		}
		/* PSA_ERROR_BAD_STATE = nothing in trial (already permanent) -> continue. */
	}

	/* === Bridge SPI FIFO-flush recovery (cold-framing self-heal) ===
	 * The transport is hardware-SS0 framed (dwc-ssi drives SS0 per transfer), but a
	 * cold first contact can still leave a 1-byte frame offset (RX FIFO residue from
	 * the master over-clocking during a desync); it cannot self-correct by more
	 * clocking -- it persists until the FIFO is flushed.  g_resync_count bursts
	 * while the slave is stuck re-arming garbage (each misframed header reads the
	 * 0xA5 idle, which is in the reserved cmd range -> re-arm++).  A burst within
	 * one ~10 ms housekeeping tick = the link is stuck: do a full SPI re-open
	 * (SPI_close/open) which FLUSHES the RX FIFO + DMA and re-arms a fresh header.
	 * If this lands in the host's inter-PING gap the slave's fresh RX then aligns
	 * with the host's next 4-byte transfer.  Healthy traffic uses valid (cmd<0x80)
	 * headers and does not bump g_resync_count, so this never fires when aligned. */
	static uint32_t last_resync;
	const uint32_t  rc = g_resync_count;
	if ((uint32_t)(rc - last_resync) >= 3u) {
		bridge_transport_spi_hw_reinit(); /* flush FIFO+DMA, re-arm clean */
		last_resync = 0u;                 /* reinit zeroes g_resync_count */
	} else {
		last_resync = rc;
	}

	/* Deferred self-reset, gated on reply_drained so the CMD_RESET ack has
	 * FULLY clocked to the host before the chip resets (audit
	 * "reset-fires-before-ack-clocked": the reset previously raced the
	 * in-flight ack and the host never saw it).  The host sees the ack,
	 * then the link goes quiet, then the firmware re-PINGs alive. */
	/* Drain any queued OTA op: the slow psa_fwu flash work runs HERE (bring-up
	 * task), never in the SPI ISR.  The flash op still stalls the CC35, so the
	 * pump stands the bridge slave down for its duration (suspend/reinit, like a
	 * radio op) -- see cc3501e_hw_ota_pump(). */
	cc3501e_hw_ota_pump();

	if (reset_pending && reply_drained) {
		NVIC_SystemReset(); /* CMSIS: M33 system reset -- does not return */
	}

	/* Deferred OTA reboot: once the FINISH ack has clocked back (reply_drained),
	 * request the PSA-FWU reboot so the cold BL2/MCUboot swaps the STAGED slot to
	 * primary (TRIAL).  Same ack-before-reboot race fix as CMD_RESET above. */
	if (ota_reboot_pending && reply_drained) {
		ota_reboot_pending = false; /* one-shot: don't re-request every tick if refused */
		/* Returns ONLY if the swap was refused (e.g. BL2 anti-rollback on a
		 * downgrade); on success it reboots and never returns.  Capture the rc so
		 * the host can distinguish "refused" from "never fired" via OTA_STATUS. */
		ota_reboot_rc = (int8_t)psa_fwu_request_reboot();
	}
}

void cc3501e_hw_request_reset(void)
{
	/* Clear reply_drained FIRST so only this command's own ack (the next
	 * reply to finish clocking) re-arms the reboot in cc3501e_hw_tick();
	 * a stale drained flag from an earlier reply must not reset early. */
	reply_drained = false;
	reset_pending = true;
}

void cc3501e_hw_notify_reply_sent(void)
{
	reply_drained = true;
}
