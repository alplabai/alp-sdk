/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- OTA firmware update (over-the-bridge
 * PSA-FWU streaming, v0.3).
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
 * All PSA-FWU access goes through cc3501e_hw_ti_ota_psa.h's plain-C seam
 * (issue #1123), not <ti/utils/FWU/psa_fwu.h> directly -- that keeps this
 * TU's state machine (the part that actually decides whether an image gets
 * staged and rebooted into) host-buildable and unit-testable; the real
 * pass-through to TI's SDK lives in cc3501e_hw_ti_ota_psa.c.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free firmware path -- but IS linked whole into
 * tests/unit/cc3501e_ota_abort_race, which supplies a host double for the
 * cc3501e_hw_ti_ota_psa.h seam.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h> /* memcpy (OTA manifest buffering) */

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* reply_drained / ota_reboot_pending / ota_reboot_rc */
#include "cc3501e_hw_ti_ota_psa.h"  /* plain-C PSA-FWU seam (issue #1123) */
#include "transport.h"              /* bridge_transport_spi_hw_reinit */

/* ===================================================================== */
/* OTA firmware update (over-the-bridge PSA-FWU streaming) -- v0.3.       */
/*                                                                       */
/* The Alif host streams a signed GPE vendor image into the non-primary  */
/* vendor slot (BEGIN -> WRITE* -> FINISH), then FINISH installs + arms a */
/* deferred reboot so the cold BL2/MCUboot swaps the slot to primary.     */
/* This is the streamed sibling of the SELFTEST cc3501e_ota_install()     */
/* (which feeds the same psa_fwu_* sequence from an embedded array).      */
/* Single session; bytes arrive sequentially (offset == cursor).         */

/* RAM-STAGED OTA (silicon-critical, hardware-SS0/READY bridge): the psa_fwu_* flash
 * ops share the CC35 HIF/DMA with the bridge SPI slave, so EVERY flash op tears
 * the bridge DMA down (like a radio op) -- doing one per 256 B WRITE disrupted the
 * phased bridge + churned the link across the ~135-chunk stream, no reinit dance made
 * it reliable (silicon 2026-06-19).  So WRITES never touch flash: each chunk is a
 * synchronous RAM memcpy into image_buf (ISR-safe, no DMA disruption -> the bulk
 * transfer stays clean).  ALL the flash happens at FINISH (psa_fwu_start + write the
 * whole staged image + install), deferred to cc3501e_hw_ota_pump() on the bring-up
 * task -- CHUNKED one CC3501E_OTA_FINISH_FLASH_BLOCK step per pump() call (issue
 * #1123, ota_finish_step() below), mirroring gd32-bridge's ota_erase_tick()
 * (firmware/gd32-bridge/src/ota.c:429-445): every step re-arms the bridge slave
 * (each step already tore the DMA down, so pump()'s single reinit call after every
 * op now fires once per chunk instead of once per whole burst) AND gives
 * cc3501e_hw_ota_abort() a checkpoint between every block instead of racing one
 * monolithic multi-KB burst.) */
#define CC3501E_OTA_IMAGE_MAX (64u * 1024u) /* max staged image; begin rejects larger */
/* FINISH flash block for the OTA-over-bridge path (distinct from the SELFTEST
 * installer's CC3501E_OTA_WRITE_CHUNK; a --ota-selftest build compiles both, so
 * they must not collide): big => few psa_fwu_write calls (each tears the bridge
 * DMA), short burst.  4096 is a multiple of the 256 B flash page. */
#define CC3501E_OTA_FINISH_FLASH_BLOCK 4096u

#define OTA_OP_IDLE     0u
#define OTA_OP_BEGIN    1u
#define OTA_OP_FINISH   3u /* WRITE is synchronous (RAM memcpy) -- not a deferred op */
#define OTA_OP_INFLIGHT 2  /* op_rc sentinel: queued, not yet executed (!= any CC3501E_HW_*) */

/* ota_finish_step() sub-phases (issue #1123) -- chunks ota_do_finish()'s old
 * monolithic flash burst across cc3501e_hw_ota_pump() calls so an ABORT
 * racing from the SPI dispatch context (see cc3501e_hw_ota_abort) gets a
 * checkpoint between every block instead of one non-preemptible call that
 * always ran to STAGED/reboot-armed before abort could be observed. */
#define OTA_FINISH_PHASE_START   0u /* psa_fwu_start, not yet run */
#define OTA_FINISH_PHASE_WRITE   1u /* streaming psa_fwu_write blocks */
#define OTA_FINISH_PHASE_INSTALL 2u /* psa_fwu_finish + psa_fwu_install */

static struct {
	uint8_t  state;  /* alp_cc3501e_ota_state_t */
	uint8_t  target; /* CC3501E_OTA_PSA_SLOT_1/2 */
	uint32_t total_len;
	uint32_t cursor; /* bytes staged into image_buf so far */
	/* Deferred BEGIN/FINISH queue (ISR enqueues; ota_pump runs the flash). */
	volatile uint8_t op;       /* OTA_OP_* currently queued/running */
	volatile int8_t  op_rc;    /* OTA_OP_INFLIGHT while pending; else result */
	uint32_t         op_total; /* BEGIN arg */
	/* FINISH chunking (issue #1123): finish_phase/finish_off step the flash
	 * burst across pump() calls -- both are touched ONLY from pump()/
	 * ota_finish_step() (task context), never from cc3501e_hw_ota_abort()
	 * (dispatch context), so they need no volatile.  abort_requested IS
	 * written from the dispatch context while pump() is mid-op: it is the
	 * ONLY channel abort() uses to influence an in-flight BEGIN/FINISH --
	 * see cc3501e_hw_ota_abort() and cc3501e_hw_ota_pump(). */
	uint8_t       finish_phase;
	uint32_t      finish_off;
	volatile bool abort_requested;
	uint8_t       image_buf[CC3501E_OTA_IMAGE_MAX]; /* full image staged in RAM */
} ota;

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
	bool    primary1 = false, primary2 = false;
	uint8_t target;

	cc3501e_ota_psa_init(); /* idempotent */
	if (!cc3501e_ota_psa_query_primary(CC3501E_OTA_PSA_SLOT_1, &primary1) ||
	    !cc3501e_ota_psa_query_primary(CC3501E_OTA_PSA_SLOT_2, &primary2)) {
		return CC3501E_HW_ERR_IO;
	}
	if (primary1 && !primary2) {
		target = CC3501E_OTA_PSA_SLOT_2;
	} else if (primary2 && !primary1) {
		target = CC3501E_OTA_PSA_SLOT_1;
	} else {
		/* Ambiguous primary (both or neither read Primary) -- a prior FAILED or
		 * aborted OTA, or an incomplete swap, can leave a slot in a TRIAL/FAILED
		 * state so the primary is unresolvable.  Do NOT bail here: that stranded the
		 * slot and made the FIRST OTA after a failure error out (and wedge the
		 * bridge) until a CC35 reset (#611).  Instead walk BOTH slots back to READY,
		 * re-query, and pick the non-primary as target (default slot 2). */
		(void)cc3501e_ota_psa_reject(); /* any STAGED -> FAILED (global) */
		(void)cc3501e_ota_psa_cancel(CC3501E_OTA_PSA_SLOT_1);
		(void)cc3501e_ota_psa_clean(CC3501E_OTA_PSA_SLOT_1);
		(void)cc3501e_ota_psa_cancel(CC3501E_OTA_PSA_SLOT_2);
		(void)cc3501e_ota_psa_clean(CC3501E_OTA_PSA_SLOT_2);
		if (cc3501e_ota_psa_query_primary(CC3501E_OTA_PSA_SLOT_2, &primary2) && primary2) {
			target = CC3501E_OTA_PSA_SLOT_1;
		} else {
			target = CC3501E_OTA_PSA_SLOT_2;
		}
	}
	bool target_queryable = false;
	if (!cc3501e_ota_psa_query_primary(target, &target_queryable)) return CC3501E_HW_ERR_IO;
	/* Walk ANY stuck state back to READY so a fresh stage always succeeds -- the
	 * common jam is a prior STAGED image the swap-reboot never promoted (e.g. a
	 * downgrade BL2 refused), and STAGED needs reject(->FAILED) BEFORE clean, not
	 * clean alone.  Mirror ota_finish_step's START phase (same order); each result
	 * is ignored when N/A.  This lets a new (forward) OTA replace a stuck pending
	 * image instead of returning BAD_STATE forever.  (`target_queryable` kept only
	 * for the fail-fast query above.) */
	(void)target_queryable;
	(void)cc3501e_ota_psa_cancel(target); /* WRITING/CANDIDATE -> FAILED */
	(void)cc3501e_ota_psa_reject();       /* STAGED           -> FAILED */
	(void)cc3501e_ota_psa_clean(target);  /* FAILED/UPDATED   -> READY  */
	ota.target    = target;
	ota.total_len = ota.op_total;
	ota.cursor    = 0u;
	ota.state     = ALP_CC3501E_OTA_STATE_WRITING;
	return CC3501E_HW_OK;
}

/* FINISH: commit the RAM-staged image to the target slot ONE flash step at a
 * time (manifest = first cc3501e_ota_psa_manifest_size() bytes -> psa_fwu_start;
 * the remainder in CC3501E_OTA_FINISH_FLASH_BLOCK-byte blocks -> psa_fwu_write;
 * finalize + install), then arm the swap-reboot.  All the OTA flash (hence all
 * bridge-DMA disruption) happens across these steps.
 *
 * Called from cc3501e_hw_ota_pump() ONCE PER TICK (issue #1123): returns
 * CC3501E_HW_BUSY while more of the sequence remains (pump() re-invokes on the
 * next tick), CC3501E_HW_OK once STAGED + the reboot latch are armed, or an
 * error.  Re-checks ota.abort_requested at the top of every call AND
 * immediately before publishing STAGED/ota_reboot_pending, so
 * cc3501e_hw_ota_abort() racing from the SPI dispatch context always gets
 * observed before either write -- the exact race issue #1123 reported (an
 * aborted session installing + rebooting anyway) is now bounded to, at worst,
 * the single non-chunked psa_fwu_finish()+psa_fwu_install() pair in the
 * INSTALL phase, which itself re-checks immediately after. */
static int ota_finish_step(void)
{
	if (ota.abort_requested) {
		return CC3501E_HW_ERR_STATE; /* cancelled; pump() unwinds to IDLE, rc is discarded */
	}

	switch (ota.finish_phase) {
	case OTA_FINISH_PHASE_START: {
		const uint32_t manifest_len = cc3501e_ota_psa_manifest_size();
		if (ota.cursor != ota.total_len || ota.total_len <= manifest_len) {
			return CC3501E_HW_ERR_INVAL;
		}
		/* Force the target component's persistent flash flow-state to READY before
		 * psa_fwu_start.  A prior failed/partial OTA leaves the flash flow-state stuck
		 * (set inside psa_fwu_start / _install), and psa_fwu_start's own flow_check then
		 * returns PSA_ERROR_BAD_STATE(-137) forever -- the RAM ComponentInfo.state can
		 * still read READY, so this must NOT be gated on it (silicon 2026-06-19).  Walk
		 * every stuck state back to READY (ignore each result -- they no-op when N/A):
		 *   cancel  WRITING/CANDIDATE -> FAILED
		 *   reject  STAGED            -> FAILED   (an install that never swap-booted)
		 *   clean   FAILED/UPDATED    -> READY
		 * (STAGED is the common stuck case here: a finish reached psa_fwu_install but
		 * the cold swap-reboot could not complete -- see project-cc3501e-firmware-bringup.) */
		(void)cc3501e_ota_psa_cancel(ota.target);
		(void)cc3501e_ota_psa_reject();
		(void)cc3501e_ota_psa_clean(ota.target);
		if (!cc3501e_ota_psa_start(ota.target, ota.image_buf, manifest_len)) {
			return CC3501E_HW_ERR_IO;
		}
		ota.finish_off   = manifest_len;
		ota.finish_phase = OTA_FINISH_PHASE_WRITE;
		return CC3501E_HW_BUSY;
	}

	case OTA_FINISH_PHASE_WRITE: {
		if (ota.finish_off >= ota.total_len) {
			ota.finish_phase = OTA_FINISH_PHASE_INSTALL;
			return CC3501E_HW_BUSY; /* one more tick before finalize+install */
		}
		uint32_t n = ota.total_len - ota.finish_off;
		if (n > CC3501E_OTA_FINISH_FLASH_BLOCK) {
			n = CC3501E_OTA_FINISH_FLASH_BLOCK;
		}
		if (!cc3501e_ota_psa_write(ota.target, ota.finish_off, &ota.image_buf[ota.finish_off], n)) {
			return CC3501E_HW_ERR_IO;
		}
		ota.finish_off += n;
		return CC3501E_HW_BUSY; /* more blocks (or INSTALL) remain */
	}

	case OTA_FINISH_PHASE_INSTALL: {
		if (!cc3501e_ota_psa_finish(ota.target)) {
			return CC3501E_HW_ERR_IO;
		}
		/* psa_fwu_install stages the swap (CANDIDATE -> STAGED); TI's
		 * PSA_SUCCESS_REBOOT return ("reboot to complete the swap") is folded into
		 * cc3501e_ota_psa_install()'s bool -- see cc3501e_hw_ti_ota_psa.c. */
		if (!cc3501e_ota_psa_install()) {
			return CC3501E_HW_ERR_IO;
		}
		if (ota.abort_requested) {
			/* Landed between the last WRITE block and here: the image already
			 * committed to flash (psa_fwu cannot unwind past install), but the
			 * two writes abort() actually promised never to race -- ota.state and
			 * the swap-reboot latch -- are still ungated at this point.  Skip
			 * them: a cancelled session must not report STAGED and must not
			 * reboot into the image the host asked to cancel (#1123). */
			return CC3501E_HW_ERR_STATE;
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

	default:
		return CC3501E_HW_ERR_INVAL; /* unreachable */
	}
}

/* Run a queued OTA op (bring-up task, NOT the SPI ISR).  Called from hw_tick.
 * The slow psa_fwu flash work runs HERE, never in the SPI ISR.
 *
 * The psa_fwu flash op writes the external xSPI image store, which shares the
 * CC35 HIF/DMA controller with the bridge SPI -- exactly like a radio op (see
 * transport_hw_ti_spi.c header), it leaves the bridge slave's DMA torn down, so
 * the link goes silent until the slave is re-opened.  Recover with the SAME
 * recover-AFTER reinit the radio path uses: run the (now single-chunk) step,
 * THEN re-open + re-arm the slave at a clean boundary.  This is recover-AFTER
 * only -- NO suspend BEFORE (SPI_transferCancel/close before the op raced the
 * live SPI callback and locked the core up; bench-proven 2026-06-19).  The
 * host poll-retries on ALP_ERR_IO across each down-window (its OTA_WRITE
 * pushes the payload once then polls header-only STATUS, so nothing is
 * half-served across the flash). */
void cc3501e_hw_ota_pump(void)
{
	if (ota.op_rc != OTA_OP_INFLIGHT) return; /* nothing queued */
	int rc;
	switch (ota.op) {
	case OTA_OP_BEGIN:
		rc = ota_do_begin();
		break;
	case OTA_OP_FINISH:
		rc = ota_finish_step(); /* ONE bounded chunk; CC3501E_HW_BUSY = more work remains */
		break;
	default:
		rc = CC3501E_HW_ERR_INVAL;
		break;
	}
	bridge_transport_spi_hw_reinit(); /* this step tore the bridge DMA down -- re-open + re-arm */
	if (rc == CC3501E_HW_BUSY) {
		/* FINISH still chunking (ota_finish_step): stay INFLIGHT so the next tick
		 * resumes where it left off, and so cc3501e_hw_ota_abort() gets a fresh
		 * checkpoint between every block instead of racing one monolithic burst. */
		return;
	}
	if (ota.abort_requested) {
		/* Cancelled while BEGIN/FINISH was in flight -- cc3501e_hw_ota_abort()
		 * deferred the state clear to us instead of racing this publish.  Unwind
		 * to IDLE regardless of rc: an aborted session must not surface as
		 * WRITING/STAGED/ERROR from a run it asked to cancel, and FINISH must not
		 * arm the swap-reboot latch (issue #1123). */
		ota.abort_requested = false;
		ota.finish_phase    = OTA_FINISH_PHASE_START;
		ota.finish_off      = 0u;
		ota.state           = ALP_CC3501E_OTA_STATE_IDLE;
		ota.cursor          = 0u;
		ota.total_len       = 0u;
		ota.op              = OTA_OP_IDLE;
		ota.op_rc           = 0;
		return;
	}
	if (rc != CC3501E_HW_OK) {
		ota.state = ALP_CC3501E_OTA_STATE_ERROR;
	}
	ota.op    = OTA_OP_IDLE; /* free the slot -- result is observable via STATUS */
	ota.op_rc = (int8_t)rc;  /* publish LAST: clears INFLIGHT so a new op can queue */
}

int cc3501e_hw_ota_begin(uint32_t total_len)
{
	if (total_len <= cc3501e_ota_psa_manifest_size() || total_len > CC3501E_OTA_IMAGE_MAX) {
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
		const int rc = (int)ota.op_rc;
		ota.state    = ALP_CC3501E_OTA_STATE_IDLE;
		ota.op_rc    = (int8_t)CC3501E_HW_OK;
		return rc;
	}
	ota.op_total = total_len; /* stage before the queue slot opens */
	return ota_submit(OTA_OP_BEGIN);
}

/* OTA_WRITE: SYNCHRONOUS -- just stage the chunk into RAM (image_buf).  No flash
 * here (that all happens at FINISH), so this is ISR-safe + causes no bridge-DMA
 * disruption: the bulk transfer stays clean across all ~135 chunks.  Idempotent
 * on the cursor so a host re-send of an already-staged chunk is harmless.
 *
 * Runs from the same single-threaded SPI dispatch context as
 * cc3501e_hw_ota_abort() (see main.c), so the two can never race each other
 * directly (issue #1123's "define the result of abort racing WRITE"): a WRITE
 * that lands after an abort simply sees ota.state != WRITING (abort always
 * clears it, whether synchronously here or via cc3501e_hw_ota_pump()'s
 * deferred unwind) and is rejected by the check below, same as any WRITE
 * without an open session. */
int cc3501e_hw_ota_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (data == 0 || len == 0u || len > (uint32_t)ALP_CC3501E_OTA_MAX_CHUNK) {
		return CC3501E_HW_ERR_INVAL;
	}
	if ((uint64_t)offset + len <= ota.cursor) return CC3501E_HW_OK; /* chunk already staged */
	if (offset != ota.cursor) return CC3501E_HW_ERR_INVAL;          /* out of order */
	if ((uint64_t)offset + len > ota.total_len || (uint64_t)offset + len > CC3501E_OTA_IMAGE_MAX) {
		return CC3501E_HW_ERR_INVAL; /* overruns the declared image / the RAM buffer */
	}
	memcpy(&ota.image_buf[offset], data, len);
	ota.cursor += len;
	return CC3501E_HW_OK;
}

int cc3501e_hw_ota_finish(void)
{
	if (ota.state == ALP_CC3501E_OTA_STATE_STAGED) return CC3501E_HW_OK; /* already finished */
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;
	ota.finish_phase = OTA_FINISH_PHASE_START; /* fresh chunk walk (ota_finish_step) */
	ota.finish_off   = 0u;
	return ota_submit(OTA_OP_FINISH);
}

int cc3501e_hw_ota_abort(void)
{
	if (ota.op_rc == OTA_OP_INFLIGHT) {
		/* A deferred BEGIN or FINISH is queued or mid-flight on the pump (task
		 * context) -- do NOT stomp its state out from under it: the old
		 * unconditional clear here raced ota_do_finish()'s STAGED/reboot_pending
		 * publish and could let an "aborted" update install and reboot anyway
		 * (issue #1123).  Request cancellation instead; cc3501e_hw_ota_pump()
		 * checks this flag before every FINISH chunk (ota_finish_step()) and
		 * before publishing either op's result, and unwinds to IDLE itself once
		 * the in-flight step finishes.  (gd32-bridge's CMD_OTA_ABORT --
		 * firmware/gd32-bridge/src/ota.c:473-477 -- gets away with a synchronous
		 * clear only because its erase is already chunked to a single page/tick
		 * with nothing left to race by the time ABORT can be dispatched; we chunk
		 * FINISH the same way here but still fence the seam with this flag rather
		 * than assume the timing works out.) */
		ota.abort_requested = true;
		return CC3501E_HW_OK;
	}
	/* Nothing running (BEGIN/FINISH not in flight): no psa_fwu_cancel needed
	 * either -- FINISH is the only thing that ever touches the target slot, and
	 * it never got there.  Safe to clear synchronously. */
	ota.state     = ALP_CC3501E_OTA_STATE_IDLE;
	ota.cursor    = 0u;
	ota.total_len = 0u;
	ota.op        = OTA_OP_IDLE;
	ota.op_rc     = 0;
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
	 * clean no-op. */
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
