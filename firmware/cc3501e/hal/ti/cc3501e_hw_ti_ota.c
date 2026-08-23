/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- OTA firmware update (over-the-bridge
 * PSA-FWU streaming, v0.2).
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  This is the host-driven streaming OTA session (BEGIN / WRITE /
 * FINISH / ABORT / PROMOTE / STATUS); the ONE-SHOT boot-time TRIAL-image
 * accept + the SELFTEST embedded-candidate installer stay in
 * cc3501e_hw_ti.c (they run from cc3501e_hw_tick(), not from a host
 * session).  cc3501e_hw_ti.c also owns the deferred-reboot latch
 * (reply_drained / ota_reboot_pending / ota_reboot_rc) this file arms --
 * see cc3501e_hw_ti_internal.h for the cross-TU seam.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h> /* memcpy + memmove (OTA window staging) */

#include <ti/devices/cc35xx/cmsis/device.h> /* CMSIS core: SysTick -- tick suppression across psa_fwu_start */
#include <ti/drivers/dpl/ClockP.h>
#include <ti/utils/FWU/psa_fwu.h> /* PSA Firmware Update: stream + install the vendor image */

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* reply_drained / ota_reboot_pending / ota_reboot_rc */
#include "transport.h"              /* bridge_transport_spi_polled + the DMA-mode re-arm */

/* ===================================================================== */
/* OTA firmware update (over-the-bridge PSA-FWU streaming) -- v0.2.       */
/*                                                                       */
/* The Alif host streams a signed GPE vendor image into the non-primary  */
/* vendor slot (BEGIN -> WRITE* -> FINISH), then FINISH installs + arms a */
/* deferred reboot so the cold BL2/MCUboot swaps the slot to primary.     */
/* This is the streamed sibling of the SELFTEST cc3501e_ota_install()     */
/* (which feeds the same psa_fwu_* sequence from an embedded array).      */
/* Single session; bytes arrive sequentially (offset == cursor).         */

/* WINDOW-STAGED OTA (silicon-critical, hardware-SS0/READY bridge).  The psa_fwu_*
 * flash ops share the CC35 HIF/DMA with the bridge SPI slave, so EVERY flash op
 * tears the bridge DMA down, like a radio op.
 *
 * v0.2 answered that by never flashing during WRITE: chunks were memcpy'd into a
 * 64 KiB whole-image buffer and ALL the flash happened once at FINISH.  That made
 * OTA reliable and also capped it at 64 KiB -- ~16x below the 1,089,100 B image
 * the channel exists to deliver (#1610).  The two were the same decision, which
 * is why raising the constant alone could never work.
 *
 * Now: WRITE still never flashes (ISR-safe memcpy, no DMA disruption), but into a
 * SLIDING WINDOW.  When the window fills, WRITE queues OTA_OP_FLUSH and returns
 * BUSY *without consuming the chunk*; the pump commits the window on the bring-up
 * task using the exact burst shape FINISH has always used and that is
 * silicon-proven at 31,428 B and 37,016 B.  FINISH flushes the tail, then
 * finalize + install as before.
 *
 * WHAT THE 2026-06-19 PER-256 B ATTEMPT ACTUALLY GOT WRONG: not the flush size.
 * psa_fwu_write is a direct XMEMWFF3_write with the SDK's SECTOR_SIZE == 4096, so
 * the flash-op count is image_bytes/4096 no matter how the flushes are grouped.
 * The difference is what the host clocks across the blackout -- that attempt kept
 * re-sending payload-bearing WRITE frames into a torn-down slave.  So the host
 * MUST send each WRITE once and then poll header-only until
 * OTA_STATUS.reserved[1] (flush_pending) clears.  The host-side change is
 * mandatory, not an optimisation; see chips/cc3501e/cc3501e_ota.c. */
/* Largest image BEGIN will accept.  This used to be the RAM staging buffer's size
 * (64 KiB), which capped OTA ~16x below the 1,089,100 B image the channel exists
 * to deliver (#1610).  Staging whole was never merely unwise -- it is impossible:
 * DRAM_NON_SECURE is 0x7f24f with ~3 KiB free and ALL RAM on the part sums to
 * 753,659 B.  With windowed staging the RAM bound is gone, so this is now only a
 * SANITY bound; the real ceiling is the vendor slot, enforced by psa_fwu_write
 * failing mid-stream (a clean ERROR, not a silent truncation).  Deliberately NOT
 * derived from a SysConfig slot constant: that value is generated and git-ignored,
 * and a silently-wrong hardcode would reproduce this exact bug one flash-layout
 * change later. */
#define CC3501E_OTA_IMAGE_MAX (2u * 1024u * 1024u)
/* FINISH flash block for the OTA-over-bridge path (distinct from the SELFTEST
 * installer's CC3501E_OTA_WRITE_CHUNK; a --ota-selftest build compiles both, so
 * they must not collide): big => few psa_fwu_write calls (each tears the bridge
 * DMA), short burst.  4096 is a multiple of the 256 B flash page. */
#define CC3501E_OTA_FINISH_FLASH_BLOCK 4096u

/* Sliding staging window (#1610).  Bytes accumulate here and are flushed to the
 * slot whenever the window fills, instead of the whole image being held in RAM.
 *
 * SIZING IS A TUNING KNOB, NOT THE FIX.  psa_fwu_write does no internal
 * buffering -- it is a direct XMEMWFF3_write, one flash op per call (SDK
 * psa_fwu.c:924) -- and the SDK's SECTOR_SIZE is 4096, the same as
 * CC3501E_OTA_FINISH_FLASH_BLOCK.  So the flash-op count is image_bytes/4096
 * (~266 for a 1.09 MB image) WHATEVER window we choose; the window only decides
 * how those ops are grouped into host hold-off episodes (16 KiB -> ~67 episodes
 * of 4 writes).  Bigger = fewer, longer stalls and more .bss; smaller = more,
 * shorter stalls.  What actually decides whether this survives is that the host
 * clocks nothing but header-only polls across each episode (see the host note in
 * ota_flush), which is the one thing the 2026-06-19 per-256 B attempt got wrong.
 *
 * 16 KiB also hands ~48 KiB of .bss back versus the old 64 KiB whole-image
 * buffer, which matters on a part with ~3 KiB of DRAM_NON_SECURE free. */
#define CC3501E_OTA_WINDOW (4u * CC3501E_OTA_FINISH_FLASH_BLOCK)

#define OTA_OP_IDLE     0u
#define OTA_OP_BEGIN    1u
#define OTA_OP_FINISH   3u
#define OTA_OP_FLUSH    4u /* window full -> commit it to the slot, off the ISR */
#define OTA_OP_INFLIGHT 2  /* op_rc sentinel: queued, not yet executed (!= any CC3501E_HW_*) */

static struct {
	uint8_t             state; /* alp_cc3501e_ota_state_t */
	psa_fwu_component_t target;
	uint32_t            total_len;
	uint32_t            cursor; /* bytes ACCEPTED from the host so far           */
	/* Deferred BEGIN/FINISH/FLUSH queue (ISR enqueues; ota_pump runs the flash). */
	volatile uint8_t op;       /* OTA_OP_* currently queued/running */
	volatile int8_t  op_rc;    /* OTA_OP_INFLIGHT while pending; else result */
	uint32_t         op_total; /* BEGIN arg */
	/* Windowed staging.  window_base is the ABSOLUTE image offset of window[0], so
	 * window_base + window_used == cursor at all times.  flushed is the absolute
	 * offset one past the last byte committed to the slot.
	 *
	 * `started` gates psa_fwu_start (which consumes the manifest -- the first
	 * TI_FWU_MANIFEST_SIZE bytes -- exactly as TI's own OTA reference does).  It
	 * MUST be cleared per session in ota_do_begin, including after a SUCCESSFUL
	 * finish: leave it latched and the second OTA in one power cycle skips
	 * psa_fwu_start entirely and writes into a slot that was never opened. */
	/* Fault latch for bench triage (#1610).  The CC3501E has NO UART on the
	 * XDS110, so a failed flush can only report itself over the bridge.
	 * fail_stage: 1=psa_fwu_start 2=psa_fwu_write 3=psa_fwu_finish
	 * 4=psa_fwu_install 5=window shorter than the manifest.
	 * fail_psa: LOW BYTE of the psa_status_t -- enough to tell the candidates
	 * apart (BAD_STATE -137 -> 0x77, INVALID_ARGUMENT -135 -> 0x79,
	 * STORAGE_FAILURE -146 -> 0x6e, NOT_PERMITTED -133 -> 0x7b). */
	uint8_t  fail_stage;
	uint8_t  fail_psa;
	uint32_t window_base;
	uint32_t window_used;
	uint32_t flushed;
	bool     started;
	/* ALIGNED: this buffer is handed straight to psa_fwu_start (manifest verify)
	 * and psa_fwu_write (slot streaming) -- both crypto/flash paths that DMA from
	 * it.  Without this the preceding members (\.\.\. flushed, bool started) leave
	 * window[] at struct offset 41, i.e. address = 1 (mod 4), and a misaligned
	 * manifest pointer is a prime suspect for psa_fwu_start hanging instead of
	 * returning.  32 bytes, not 4: the same buffer feeds DMA/cache-line paths. */
	uint8_t window[CC3501E_OTA_WINDOW] __attribute__((aligned(32)));
} ota;

/* The window must hold the whole manifest, because the first flush hands
 * window[0..TI_FWU_MANIFEST_SIZE) to psa_fwu_start before writing any image
 * bytes, and it must be a whole number of flash blocks so every mid-stream flush
 * is block-aligned (only the final flush at FINISH may be partial). */
_Static_assert(CC3501E_OTA_WINDOW >= (uint32_t)TI_FWU_MANIFEST_SIZE,
               "OTA window must hold the manifest psa_fwu_start consumes");
_Static_assert((CC3501E_OTA_WINDOW % CC3501E_OTA_FINISH_FLASH_BLOCK) == 0u,
               "OTA window must be a whole number of flash blocks");

/* Enqueue op @o (args already staged) and return BUSY: an op is in flight while
 * op_rc == OTA_OP_INFLIGHT.  The pump publishes the result + frees the slot
 * (auto-resets op to IDLE); the host observes completion through OTA_STATUS
 * (state / cursor), NOT by re-collecting -- so a WRITE poll never has to re-send
	 * its 256 B payload while the device is mid-flash (which would disrupt the
	 * phased bridge during the flash blackout).  Fast + ISR-safe (no flash here). */
static int ota_submit(uint8_t o)
{
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY; /* an op is running */
	ota.op    = o;
	ota.op_rc = OTA_OP_INFLIGHT;
	return CC3501E_HW_BUSY;
}

/* ---- slow bodies (run ONLY from ota_pump, off the SPI ISR) ----------------- */

static int ota_do_begin(void)
{
	psa_fwu_component_info_t i1 = { 0 }, i2 = { 0 }, ti = { 0 };
	psa_fwu_component_t      target;

	psa_fwu_init(); /* idempotent */
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_1, &i1) != PSA_SUCCESS ||
	    psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_2, &i2) != PSA_SUCCESS) {
		return CC3501E_HW_ERR_IO;
	}
	if (i1.impl.Primary && !i2.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_2;
	} else if (i2.impl.Primary && !i1.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_1;
	} else {
		/* Ambiguous primary (both or neither read Primary) -- a prior FAILED or
		 * aborted OTA, or an incomplete swap, can leave a slot in a TRIAL/FAILED
		 * state so the primary is unresolvable.  Do NOT bail here: that stranded the
		 * slot and made the FIRST OTA after a failure error out (and wedge the
		 * bridge) until a CC35 reset (#611).  Instead walk BOTH slots back to READY,
		 * re-query, and pick the non-primary as target (default slot 2). */
		(void)psa_fwu_reject(PSA_ERROR_GENERIC_ERROR); /* any STAGED -> FAILED (global) */
		(void)psa_fwu_cancel((psa_fwu_component_t)Vendor_Image_Slot_1);
		(void)psa_fwu_clean((psa_fwu_component_t)Vendor_Image_Slot_1);
		(void)psa_fwu_cancel((psa_fwu_component_t)Vendor_Image_Slot_2);
		(void)psa_fwu_clean((psa_fwu_component_t)Vendor_Image_Slot_2);
		if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_2, &i2) == PSA_SUCCESS &&
		    i2.impl.Primary) {
			target = (psa_fwu_component_t)Vendor_Image_Slot_1;
		} else {
			target = (psa_fwu_component_t)Vendor_Image_Slot_2;
		}
	}
	if (psa_fwu_query(target, &ti) != PSA_SUCCESS) return CC3501E_HW_ERR_IO;
	/* Walk ANY stuck state back to READY so a fresh stage always succeeds -- the
	 * common jam is a prior STAGED image the swap-reboot never promoted (e.g. a
	 * downgrade BL2 refused), and STAGED needs reject(->FAILED) BEFORE clean, not
	 * clean alone.  Mirror ota_do_finish's recovery (same order); each rc no-ops
	 * when N/A.  This lets a new (forward) OTA replace a stuck pending image
	 * instead of returning BAD_STATE forever.  (`ti` kept for the query above.) */
	/* Prepare the slot ONLY when its state needs it -- TI's OTA_FWU_prepareSlot
	 * (examples/rtos/LP_EM_CC35X1/demos/ota_example/ota_fwu.c:119) branches on
	 * psa_fwu_query's state and does NOTHING when the slot is already READY.
	 * Running cancel+reject+clean unconditionally erased a ~1 MB slot on EVERY
	 * BEGIN -- up to three erases back to back -- which is why BEGIN grew from
	 * 52 ms to 22 s to 41 s to a >11 min hang as the slot accumulated dirt across
	 * aborted sessions.  TI's own example prints "erasing flash, please wait..."
	 * for exactly this call, i.e. the erase is expected to be slow and is
	 * therefore expected to be RARE. */
	if (ti.state != PSA_FWU_READY) {
		if (ti.state == PSA_FWU_WRITING || ti.state == PSA_FWU_CANDIDATE) {
			(void)psa_fwu_cancel(target); /* -> FAILED */
			(void)psa_fwu_clean(target);  /* -> READY  */
		} else if (ti.state == PSA_FWU_STAGED || ti.state == PSA_FWU_TRIAL) {
			(void)psa_fwu_reject(PSA_ERROR_GENERIC_ERROR); /* -> FAILED */
			(void)psa_fwu_clean(target);                   /* -> READY  */
		} else {
			(void)psa_fwu_clean(target); /* FAILED / UPDATED -> READY */
		}
	}
	ota.target    = target;
	ota.total_len = ota.op_total;
	ota.cursor    = 0u;
	/* Reset EVERY per-session field, not just the pre-#1610 four.  A stale
	 * `started`/`flushed` here is the difference between a clean second update and
	 * one that writes into a slot psa_fwu_start never opened, at an offset carried
	 * over from the previous image. */
	ota.window_base = 0u;
	ota.window_used = 0u;
	ota.flushed     = 0u;
	ota.started     = false;
	ota.fail_stage  = 0u;
	ota.fail_psa    = 0u;
	ota.state       = ALP_CC3501E_OTA_STATE_WRITING;
	return CC3501E_HW_OK;
}

/* Commit the block-aligned prefix of the window to the target slot.  This is the
 * ONLY place OTA touches flash besides finalize, and it is a deliberate clone of
 * the burst shape ota_do_finish has always used and that is silicon-proven at
 * 31,428 B and 37,016 B: CC3501E_OTA_FINISH_FLASH_BLOCK writes with a bridge
 * re-arm every 2nd block, on the bring-up task, never the SPI ISR.
 *
 * @p final is true only for the last flush (from FINISH), where a short trailing
 * block is expected and allowed; mid-stream flushes commit whole blocks only and
 * carry the remainder forward, so psa_fwu_write always sees a block-sized write
 * except at the very end.
 *
 * HOST CONTRACT: the host must clock ONLY header-only polls while this runs.
 * That is the single thing the 2026-06-19 per-256 B attempt got wrong -- it kept
 * re-sending payload-bearing WRITE frames into a slave whose DMA was torn down.
 * cc3501e_hw_ota_flush_pending() exists so the host can see this window and wait
 * it out instead of guessing from BUSY. */
static int ota_flush(bool final)
{
	if (!ota.started) {
		if (ota.window_used < (uint32_t)TI_FWU_MANIFEST_SIZE) {
			ota.fail_stage = 5u;
			return CC3501E_HW_ERR_INVAL; /* cannot open the slot without a manifest */
		}
		/* NO slot walk here.  ota_do_begin already brought this slot to READY,
		 * so repeating cancel+reject+clean made the FIRST FLUSH a SECOND full
		 * ~1 MB erase -- the longest blackout in the session, landing exactly at
		 * off=16384 where every run died.  TI's reference erases once, in
		 * prepareSlot, and then only streams. */
		/* NOT interrupt-masked.  I masked this on audit advice ("psa_fwu_start
		 * tears the OTFDE down globally") and kept it even after the SAME mask on
		 * the cancel/clean walk was proven to hang BEGIN for >11 min.  Bisecting
		 * the flush pinned the hang here: skipping psa_fwu_start made the flush
		 * complete in 1 ms with the link ALIVE (status=0, flush=0), while with it
		 * the device answers once and is never heard from again.  psa_fwu_start
		 * verifies the manifest and opens the slot -- crypto and flash that are
		 * interrupt-driven -- so masking across it deadlocks. */
		/* TRIED AND REJECTED 2026-08-21: suppressing ONLY the RTOS tick
		 * (SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk) across this call did NOT
		 * fix the hang, and it is actively unsafe -- ClockP on this SDK is built
		 * on FreeRTOS software timers, so any ClockP wait inside psa_fwu_start
		 * would then never expire, adding a SECOND hang mode on top of the one
		 * being diagnosed.  Do not retry it.  (The observation that motivated it
		 * still stands and is still unexplained: SysTick_Handler 0x140c9838,
		 * PendSV_Handler 0x140a6f70 and SVC_Handler 0x140c97f0 all execute from
		 * XIP FLASH, while this function tears the OTFDE down globally.) */
		const psa_status_t ps = psa_fwu_start(ota.target, ota.window, TI_FWU_MANIFEST_SIZE);
		if (ps != PSA_SUCCESS) {
			ota.fail_stage = 1u;
			ota.fail_psa   = (uint8_t)((uint32_t)ps & 0xFFu);
			return CC3501E_HW_ERR_IO;
		}
		ota.started = true;
		/* psa_fwu_write REJECTS any offset below the manifest (SDK psa_fwu.c:919,
		 * PSA_ERROR_INVALID_ARGUMENT), so image bytes start HERE.  Pre-#1610 this
		 * anchor lived only in ota_do_finish's loop initialiser; losing it is why a
		 * naive flush cursor starting at 0 fails on its very first write. */
		ota.flushed = (uint32_t)TI_FWU_MANIFEST_SIZE;
	}

	const uint32_t end     = ota.window_base + ota.window_used; /* == ota.cursor */
	uint32_t       n_ready = (end > ota.flushed) ? (end - ota.flushed) : 0u;
	if (!final) {
		const uint32_t whole = n_ready & ~(CC3501E_OTA_FINISH_FLASH_BLOCK - 1u);
		/* Rounding to whole blocks is right until the window is FULL: then a
		 * remainder below one block would commit nothing, ota_flush would return
		 * OK with the window still full, and the next WRITE would re-queue FLUSH
		 * forever -- progress stops with no error anywhere.  Drain the partial
		 * tail in that case (psa_fwu_write accepts a partial write). */
		n_ready =
		    (whole == 0u && ota.window_used >= (uint32_t)CC3501E_OTA_WINDOW) ? n_ready : whole;
	}
	if (n_ready == 0u) return CC3501E_HW_OK;

	uint32_t       off  = ota.flushed;
	const uint32_t stop = ota.flushed + n_ready;
	/* SNAPSHOT window_base for the whole loop.  cc3501e_hw_ota_abort() deliberately
	 * has NO in-flight guard (it is the host's only escape from a stuck pump) and it
	 * zeroes window_base -- in NORMAL mode from the SPI callback/ISR, which does
	 * preempt this loop.  Re-reading it per iteration would let an abort landing
	 * mid-burst turn `off - ota.window_base` into a ~1 MB index and hand
	 * psa_fwu_write a wild pointer.  With the snapshot the worst case is the one
	 * abort's own comment already accepts as rare and self-correcting: some stale
	 * window bytes reach a slot the next ota_do_begin walks back to READY.  (In
	 * update mode the whole polled loop is one task, so there is nothing to race --
	 * the snapshot costs nothing and keeps both modes reading the same.) */
	const uint32_t base = ota.window_base;
	while (off < stop) {
		uint32_t n = stop - off;
		if (n > CC3501E_OTA_FINISH_FLASH_BLOCK) n = CC3501E_OTA_FINISH_FLASH_BLOCK;
		const psa_status_t pw = psa_fwu_write(ota.target, off, &ota.window[off - base], n);
		if (pw != PSA_SUCCESS) {
			ota.fail_stage = 2u;
			ota.fail_psa   = (uint8_t)((uint32_t)pw & 0xFFu);
			return CC3501E_HW_ERR_IO;
		}
		off += n;
		/* NO mid-burst reinit.  Re-opening the SPI slave BETWEEN psa_fwu_write
		 * calls interleaves a bus teardown with an in-progress flash burst -- the
		 * same class of interleaving that made SPI_transferCancel hang the bridge.
		 * The host is holding off for the whole flush anyway (it polls header-only
		 * and never clocks payload), so there is nothing to serve here; the pump's
		 * single bridge_transport_spi_hw_reinit() after the op is what restores the
		 * link.  Silicon 2026-08-21: with the mid-burst re-arm in, the FIRST flush
		 * never cleared -- flush_pending was still 1 after 600 s with the device
		 * reporting dev_state=1 (WRITING) dev_cursor=16384. */
	}
	ota.flushed = stop;

	/* Carry any un-flushed tail to the front so the window is reusable.  memmove,
	 * not memcpy: the regions overlap whenever the tail is longer than the gap. */
	const uint32_t keep = end - ota.flushed;
	if (keep != 0u) {
		memmove(ota.window, &ota.window[ota.flushed - base], keep); /* base: see above */
	}
	ota.window_base = ota.flushed;
	ota.window_used = keep;
	/* No reinit here: cc3501e_hw_ota_pump() already provides the clean boundary
	 * after the op, and doing it twice back-to-back with no flash op in between
	 * doubles the SPI_open DMA-race rolls -- the second close lands on a freshly
	 * ARMED slave, which is the one teardown that had no preceding flash op. */
	return CC3501E_HW_OK;
}

/* FINISH: flush whatever is still sitting in the window, then finalize + install
 * and arm the swap-reboot.  Most of the image is ALREADY in the slot by now --
 * the windowed design flushes as it streams (ota_flush), with the manifest going
 * to psa_fwu_start on the first flush and every later block to psa_fwu_write in
 * CC3501E_OTA_FINISH_FLASH_BLOCK pages.  This is no longer "all the OTA flash in
 * one burst"; it is the tail plus the commit. */
static int ota_do_finish(void)
{
	if (ota.cursor != ota.total_len || ota.total_len <= (uint32_t)TI_FWU_MANIFEST_SIZE) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* NOTE: the walk-back-to-READY that used to sit here (cancel/reject/clean before
	 * psa_fwu_start, silicon 2026-06-19) moved into ota_flush's first-flush branch,
	 * because the slot is now opened at the FIRST flush rather than at FINISH.  It
	 * still runs exactly once per session, in the same order, immediately before
	 * psa_fwu_start -- see ota_flush.
	 *
	 * Commit whatever is still in the window (short trailing block allowed).  For a
	 * small image this is the FIRST flush too, so it still opens the slot -- the
	 * pre-#1610 single-burst behaviour is preserved exactly for images that never
	 * filled the window, which is every image the bench has proven so far. */
	const int fr = ota_flush(true);
	if (fr != CC3501E_HW_OK) return fr;
	if (ota.flushed != ota.total_len) return CC3501E_HW_ERR_IO; /* staged short */
	psa_status_t pf = psa_fwu_finish(ota.target);
	if (pf != PSA_SUCCESS && pf != PSA_SUCCESS_REBOOT) {
		ota.fail_stage = 3u;
		ota.fail_psa   = (uint8_t)((uint32_t)pf & 0xFFu);
		return CC3501E_HW_ERR_IO;
	}
	/* psa_fwu_install stages the swap and returns PSA_SUCCESS_REBOOT(1) -- a SUCCESS
	 * code meaning "reboot to complete the swap", NOT an error. */
	psa_status_t pi = psa_fwu_install(); /* CANDIDATE -> STAGED */
	if (pi != PSA_SUCCESS && pi != PSA_SUCCESS_REBOOT) {
		ota.fail_stage = 4u;
		ota.fail_psa   = (uint8_t)((uint32_t)pi & 0xFFu);
		return CC3501E_HW_ERR_IO;
	}
	ota.state = ALP_CC3501E_OTA_STATE_STAGED;
	/* Arm the standard swap-reboot: the tick calls psa_fwu_request_reboot once the
	 * FINISH ack has drained -> the device reboots, BL2 swaps the STAGED slot to
	 * primary (TRIAL), the new image boots and self-accepts (cc3501e_hw_tick).  This
	 * is the production OTA contract.  (On the current mis-activated bench unit the
	 * swap-boot is gated by the vendor-SBL cold-boot issue -- see
	 * project-cc3501e-ota-bridge-rootcause -- but the receive/stage/install pipeline
	 * up to STAGED is silicon-validated.) */
	reply_drained      = false;
	ota_reboot_pending = true;
	return CC3501E_HW_OK;
}

/* Run a queued OTA op (bring-up task, NOT the SPI ISR).  Called from hw_tick.
 * The slow psa_fwu flash work runs HERE, never in the SPI ISR.
 *
 * The psa_fwu flash op writes the external xSPI image store, which shares the
 * CC35 HIF/DMA controller with the bridge SPI -- exactly like a radio op (see
 * transport_hw_ti_spi.c header), it leaves the bridge slave's DMA torn down, so
 * the link goes silent until the slave is re-opened.  Recover with the SAME
 * recover-AFTER reinit the radio path uses: run the op, THEN re-open + re-arm the
 * slave at a clean boundary.  This is recover-AFTER only -- NO suspend BEFORE
 * (SPI_transferCancel/close before the op raced the live SPI callback and locked
 * the core up; bench-proven 2026-06-19).  The host poll-retries on ALP_ERR_IO
 * across the down-window (its OTA_WRITE pushes the payload once then polls
 * header-only STATUS, so nothing is half-served across the flash).
 *
 * ALL of that is the NORMAL (DMA) bridge.  In OTA UPDATE MODE the slave is open
 * SPI_MODE_BLOCKING, no DMA transaction is ever primed, and the flash op disrupts
 * nothing -- so there is nothing to release and nothing to recover, and the
 * teardown would itself be the fault (see the omission notes in the body).
 *
 * Both modes reach this function through cc3501e_hw_tick().  In UPDATE MODE that
 * tick runs on the SAME task as the polled bridge, BETWEEN two frames (src/main.c)
 * -- deliberately, because a frame boundary is the only point at which the SPI
 * FIFOs can be reset without eating a half-consumed phase, and that reset
 * (spi_fifo_reset, hal/ti/transport_hw_ti_spi.c) is what makes this blackout
 * survivable.  Nothing here may touch the bridge in polled mode: not the handle,
 * and no READY RAISE -- poll_service owns that line end-to-end there. */

void cc3501e_hw_ota_pump(void)
{
	if (ota.op_rc != OTA_OP_INFLIGHT) return; /* nothing queued */

	/* OTA UPDATE MODE.  Silicon 2026-08-21: a SPI_MODE_CALLBACK (DMA) SPI_open()
	 * PERMANENTLY prevents psa_fwu_start()/psa_fwu_write() from returning, and
	 * SPI_close() does NOT undo the claim -- so the ONLY context in which this pump
	 * completes at all is a boot on which the slave was never opened in callback
	 * mode.  On such a boot the transport is open SPI_MODE_BLOCKING /
	 * SPI_WAIT_FOREVER and there is nothing here to tear down: every teardown below
	 * is skipped, and each one is not merely useless but actively harmful (see the
	 * omission notes).  Asked of the transport at RUNTIME, never through a build
	 * switch -- one image carries both modes, so the device reboots out of update
	 * mode straight back onto the DMA bridge with the radio alive. */
	const bool polled = bridge_transport_spi_polled();

	/* Drop READY (LOW = busy) for the WHOLE flash op, exactly as worker.c brackets
	 * a radio op.  Without this the OTA path was the ONLY blackout the host was
	 * never told about: main.c raises READY at boot and radio ops leave it HIGH,
	 * so the line sat HIGH through every psa_fwu call -- the device actively
	 * telling the host "I am armed, clock away" while its slave DMA was torn
	 * down.  The host duly clocked into a dead slave, which is what desynced and
	 * wedged the bridge (silicon 2026-08-21: failures at off=0 AND off=16384,
	 * begin timings of 52 / 257 / 2292 ms and a -1 refusal, all from the same
	 * build -- it depended purely on whether a host request landed inside the
	 * flash window).  Radio ops were reliable precisely because they ARE
	 * bracketed.
	 *
	 * Unconditional -- READY LOW is TRUE in both modes here: in update mode this
	 * runs between two frames with no polled transfer armed, so "not armed" is
	 * exactly what the line should say.  Only the paired RAISE at the end of this
	 * function is !polled-guarded, because raising it with nothing armed is the lie
	 * that produced ~26 B/s with every host WRITE answering -5; in polled mode
	 * bridge_transport_spi_poll_service() re-raises it with a transfer actually
	 * armed. */
	cc3501e_bridge_busy();
	if (!polled) {
		/* NORMAL (DMA) bridge: RELEASE the slave's DMA before ANY flash work,
		 * exactly as the radio path does.  Proven on silicon 2026-08-21:
		 * psa_fwu_start called at BOOT -- before transport_spi_init() ever arms the
		 * slave -- RETURNS promptly (PSA_ERROR_BAD_STATE 0x77, a legitimate
		 * refusal), while the SAME call from this pump with the slave armed never
		 * returns at all.  The difference is the live SPI_MODE_CALLBACK transfer
		 * and its DMA contending with the flash engine.  NOTE this is
		 * bridge_transport_spi_hw_release(), NOT suspend(): suspend calls
		 * SPI_transferCancel, which hung the bridge here too (BEGIN went
		 * unreachable on both attempts).  A plain SPI_close frees the DMA. */
		bridge_transport_spi_hw_quiesce(true);
		ClockP_usleep(20000u); /* let any in-flight transfer retire; host is held off */
		bridge_transport_spi_hw_release();
	}

	int rc;
	switch (ota.op) {
	case OTA_OP_BEGIN:
		rc = ota_do_begin();
		break;
	case OTA_OP_FINISH:
		rc = ota_do_finish();
		break;
	case OTA_OP_FLUSH:
		rc = ota_flush(false);
		break;
	default:
		rc = CC3501E_HW_ERR_INVAL;
		break;
	}
	if (rc != CC3501E_HW_OK && rc != CC3501E_HW_BUSY) {
		ota.state = ALP_CC3501E_OTA_STATE_ERROR;
	}
	/* A successful FINISH arms its OWN swap reboot (ota_do_finish sets
	 * ota_reboot_pending), and the image BL2 swaps in must come up on the NORMAL
	 * DMA bridge -- an update-mode boot runs nothing but the polled bridge loop and
	 * would be deaf to the radio.  Disarm BEFORE the tick fires
	 * psa_fwu_request_reboot().
	 *
	 * UNCONDITIONAL, exactly like cc3501e_hw_ota_promote().  This flag governs the
	 * NEXT boot, so gating it on THIS boot's mode is a category error: a 0x47 arm
	 * whose warm reset has not fired yet (the tick runs this pump BEFORE both reboot
	 * latches) would otherwise survive into the swap reboot and bring the swapped
	 * image up deaf to the radio.  Clearing an already-clear flag is free. */
	if (ota.op == OTA_OP_FINISH && rc == CC3501E_HW_OK) {
		bridge_transport_spi_clear_polled_boot();
	}

	if (!polled) {
		bridge_transport_spi_hw_quiesce(false);
		bridge_transport_spi_hw_reinit(); /* flash tore the DMA down -- re-open + re-arm */
	}
	/* POLLED: no quiesce, no reinit, no re-arm.  quiesce is not merely useless here,
	 * it is HARMFUL -- arm_transfer() returns early while quiesced, so a phase
	 * recorded at the end of a frame is silently DROPPED and the host desyncs
	 * mid-frame.  reinit re-rolls SPI_open's retry dice for no gain (nothing was
	 * torn down), and closing the handle while a task sits inside a polled
	 * SPI_transfer is the deadlock that killed the earlier dedicated poll task --
	 * keep both no-ops even though today's single-task loop calls this between
	 * frames.  The re-arm belongs to the caller's next
	 * bridge_transport_spi_poll_service(), which owns the polled phase machine. */

	ota.op    = OTA_OP_IDLE; /* free the slot -- result is observable via STATUS */
	ota.op_rc = (int8_t)rc;  /* publish LAST: clears INFLIGHT so a new op can queue */
	/* Raise READY only now: once it is HIGH the host may clock immediately, so the
	 * re-arm AND the published result must both already be in place (same ordering
	 * rule worker.c documents for its DONE/ERR slot).  In POLLED mode READY has
	 * exactly ONE owner -- poll_service raises it with a transfer actually armed and
	 * drops it again on completion.  Raising it from here, with nothing armed, is
	 * precisely the lie that produced ~26 B/s with every host WRITE answering -5. */
	if (!polled) {
		cc3501e_bridge_ready();
	}
}

int cc3501e_hw_ota_begin(uint32_t total_len)
{
	if (total_len <= (uint32_t)TI_FWU_MANIFEST_SIZE || total_len > CC3501E_OTA_IMAGE_MAX) {
		return CC3501E_HW_ERR_INVAL; /* too small to hold a manifest, or larger than the RAM buffer */
	}
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;             /* op running */
	if (ota.state == ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_OK; /* already begun */
	if (ota.state == ALP_CC3501E_OTA_STATE_ERROR) {
		/* The deferred begin (ota_do_begin, on the pump) FAILED -- e.g. the
		 * psa_fwu vendor slots could not be resolved (query failed / ambiguous
		 * primary).  Surface the REAL error to the host and clear the latch so a
		 * later BEGIN starts fresh.  WITHOUT this, op_rc is no longer INFLIGHT and
		 * state is not WRITING, so each host poll_by_repeat re-submit re-runs the
		 * failing op and only ever sees BUSY -> the host times out (ALP_ERR_TIMEOUT)
		 * instead of the true cause -- bench-observed 2026-06-21 (-4 on a unit whose
		 * activation left the OTA slots unresolvable). */
		/* Clear the latch and START A FRESH SESSION rather than returning the
		 * stale rc: replaying it CONSUMED the BEGIN without opening anything, so
		 * the host saw a hard failure ("OTA begin -> -1") for an error belonging to
		 * a PREVIOUS session and had to issue a second BEGIN to get anywhere.  The
		 * real cause stays readable via OTA_STATUS fail_stage/fail_psa. */
		ota.state = ALP_CC3501E_OTA_STATE_IDLE;
		ota.op_rc = (int8_t)CC3501E_HW_OK;
	}
	ota.op_total = total_len; /* stage before the queue slot opens */
	return ota_submit(OTA_OP_BEGIN);
}

/* OTA_WRITE: SYNCHRONOUS -- copy the chunk into the SLIDING WINDOW (ota.window),
 * not into a whole-image RAM buffer.  image_buf is gone: staging the full 1.09 MB
 * image never fit (#1610), so the window fills to CC3501E_OTA_WINDOW and the
 * chunk that fills it submits a FLUSH instead of being consumed.
 *
 * So flash CAN happen mid-stream, and the host must cooperate: a submitted flush
 * publishes flush_pending in OTA_STATUS reserved[1], and the host holds off ALL
 * payload -- polling header-only -- until it clears, then re-sends the same
 * chunk.  Idempotent on the cursor, so a re-send the device already took is
 * harmless. */
int cc3501e_hw_ota_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (data == 0 || len == 0u || len > (uint32_t)ALP_CC3501E_OTA_MAX_CHUNK) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* A flush is running on the pump and OWNS the window -- never touch it
	 * concurrently.  In NORMAL mode this runs on the SPI ISR and the pump on the
	 * bring-up task, so this one guard IS the fence and no mutex is needed around
	 * the window; in update mode both run on the same task.  Returning BUSY without
	 * consuming keeps the cursor honest, so the host's re-send of this same chunk
	 * after the stall is a plain retry. */
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;
	if ((uint64_t)offset + len <= ota.cursor) return CC3501E_HW_OK; /* chunk already staged */
	if (offset != ota.cursor) return CC3501E_HW_ERR_INVAL;          /* out of order */
	if ((uint64_t)offset + len > ota.total_len) {
		return CC3501E_HW_ERR_INVAL; /* overruns the declared image */
	}
	/* Window full for this chunk?  Queue the flush and return BUSY WITHOUT
	 * consuming, so `cursor` keeps meaning "bytes accepted" and the idempotency
	 * short-circuit above stays truthful: accepted bytes are either still in the
	 * window or already committed to the slot, never lost in between. */
	if (len > (uint32_t)CC3501E_OTA_WINDOW - ota.window_used) {
		return ota_submit(OTA_OP_FLUSH);
	}
	memcpy(&ota.window[ota.window_used], data, len);
	ota.window_used += len;
	ota.cursor += len;
	/* Open the slot as soon as the MANIFEST has arrived, rather than waiting for
	 * the first FULL window.  psa_fwu_start only needs the first
	 * TI_FWU_MANIFEST_SIZE bytes, and opening early means a bad manifest is
	 * rejected after ~256 B instead of after buffering 16 KiB -- which also
	 * matches TI's reference, where the slot is opened in prepareSlot rather than
	 * mid-stream.  Queuing FLUSH here runs the same first-flush branch with only
	 * ~256 B buffered: the block-rounding yields n_ready == 0, so psa_fwu_start
	 * runs and no psa_fwu_write follows.  Returning BUSY after consuming is safe
	 * because the idempotency short-circuit above (offset + len <= ota.cursor ->
	 * OK) absorbs the host's re-send of this same chunk. */
	if (!ota.started && ota.window_used >= (uint32_t)TI_FWU_MANIFEST_SIZE) {
		return ota_submit(OTA_OP_FLUSH);
	}
	return CC3501E_HW_OK;
}

/* True while a window flush is queued or running, i.e. while the host must hold
 * off payload-bearing frames and poll header-only.  Published through
 * OTA_STATUS so the host WAITS on an explicit signal instead of inferring a
 * stall from BUSY -- BUSY alone cannot distinguish "flushing" from "another op".
 * Derived from the existing volatile op/op_rc pair, so no new shared state. */
bool cc3501e_hw_ota_flush_pending(void)
{
	/* ANY in-flight pump op is a flash blackout the host must wait out -- BEGIN
	 * (psa_fwu_cancel/reject/clean + psa_fwu_start = a slot erase, measured at
	 * 22-41 s on silicon) and FINISH every bit as much as FLUSH.  Reporting only
	 * FLUSH left the host with NO busy signal across the two longest blackouts in
	 * the session, so it could not tell "erasing a slot" from "link dead" and
	 * timed out on a perfectly healthy device. */
	return ota.op_rc == OTA_OP_INFLIGHT;
}

/* Bench triage (#1610): which psa_fwu_* call failed, and its status low byte. */
void cc3501e_hw_ota_fault(uint8_t *stage, uint8_t *psa_lo)
{
	if (stage != 0) *stage = ota.fail_stage;
	if (psa_lo != 0) *psa_lo = ota.fail_psa;
}

int cc3501e_hw_ota_finish(void)
{
	if (ota.state == ALP_CC3501E_OTA_STATE_STAGED) return CC3501E_HW_OK; /* already finished */
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;
	return ota_submit(OTA_OP_FINISH);
}

int cc3501e_hw_ota_abort(void)
{
	/* Pre-#1610 this said "an aborted session never opened one", because FINISH was
	 * the only thing that touched the slot.  That is NO LONGER TRUE: the first
	 * window flush calls psa_fwu_start, so a session aborted mid-stream can leave
	 * the slot open in WRITING.
	 *
	 * Deliberately NOT made a deferred pump op that cancels the slot: that would
	 * make ABORT able to return BUSY, a wire-semantics change on the one command a
	 * host reaches for when things are already wrong.  Instead the RAM session is
	 * reset here and the open slot is cleaned by ota_do_begin's walk-back on the
	 * NEXT begin -- the same recovery that #611 hardened, which already handles a
	 * slot left in WRITING/CANDIDATE/STAGED.  An abort while a flush is in flight
	 * is refused rather than racing the pump for the window.
	 *
	 * ...EXCEPT that refusing it left NO host-reachable escape from a stuck pump:
	 * BEGIN, WRITE, FINISH and ABORT all return BUSY while op_rc is INFLIGHT, so a
	 * pump that never completes strands the session until a power cycle.  ABORT is
	 * precisely the command a host reaches for when things are already wrong, so it
	 * now always clears the RAM session.  The window race this guarded against is a
	 * rare, self-correcting overwrite -- the next ota_do_begin walks the slot back
	 * to READY regardless -- and is far cheaper than an unrecoverable session. */
	ota.state       = ALP_CC3501E_OTA_STATE_IDLE;
	ota.cursor      = 0u;
	ota.total_len   = 0u;
	ota.window_base = 0u;
	ota.window_used = 0u;
	ota.flushed     = 0u;
	ota.started     = false;
	ota.fail_stage  = 0u;
	ota.fail_psa    = 0u;
	ota.op          = OTA_OP_IDLE;
	ota.op_rc       = 0;
	return CC3501E_HW_OK;
}

int8_t cc3501e_hw_ota_reboot_rc(void)
{
	return ota_reboot_rc;
}

int cc3501e_hw_ota_promote(void)
{
	/* Promote an ALREADY-committed pending image: arm the same deferred swap-reboot
	 * the FINISH path uses.  A STAGED image survives a bare nRESET (which carries no
	 * swap request) with the RAM session state reset to IDLE, so ota.state cannot
	 * gate this -- the host calls it deliberately when a pending image is jammed in
	 * the slot (a fresh FINISH is unreachable while a slot is occupied).  The tick
	 * fires psa_fwu_request_reboot() once this reply drains; BL2/MCUboot then swaps
	 * the pending slot to primary (TRIAL).  If nothing is pending the reboot is a
	 * clean no-op.
	 *
	 * Like FINISH, this reboot must land in the NORMAL DMA bridge, so disarm any
	 * pending update-mode boot first.  Unconditional: clearing an already-clear flag
	 * is free, and no swap reboot ever WANTS update mode. */
	bridge_transport_spi_clear_polled_boot();
	reply_drained      = false;
	ota_reboot_pending = true;
	return CC3501E_HW_OK;
}

int cc3501e_hw_ota_status(uint8_t *state, uint32_t *bytes_written, uint32_t *total_len)
{
	if (state != 0) *state = ota.state;
	if (bytes_written != 0) *bytes_written = ota.cursor;
	if (total_len != 0) *total_len = ota.total_len;
	return CC3501E_HW_OK;
}
