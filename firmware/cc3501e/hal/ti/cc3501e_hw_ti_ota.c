/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- OTA firmware update (over-the-bridge
 * PSA-FWU streaming, v0.4).
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
/* OTA firmware update (over-the-bridge PSA-FWU streaming) -- v0.4.       */
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
 * monolithic multi-KB burst.  "pump() runs once per tick" is a convenience label,
 * not a timing guarantee -- see cc3501e_hw_ota_pump()'s header. */
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
	/* FINISH chunking (issue #1123): touched ONLY from pump()/ota_finish_step()
	 * (task context), never from cc3501e_hw_ota_abort() (dispatch context), so
	 * neither needs atomics -- the abort<->pump handoff itself is the separate
	 * ota_op_gen/ota_abort_gen pair below. */
	uint8_t  finish_phase;
	uint32_t finish_off;
	uint8_t  image_buf[CC3501E_OTA_IMAGE_MAX]; /* full image staged in RAM */
} ota;

/* Cross-context abort handoff (issue #1123 round 2).  A single `abort_requested`
 * bool raced two ways against pump()'s publish: (a) abort() could set it AFTER
 * pump() had already read it false for this tick but BEFORE pump() finished
 * publishing, so the flag survived, unconsumed, into whatever op was submitted
 * NEXT; (b) within pump()'s own unwind branch, a fresh cc3501e_hw_ota_abort()
 * call landing between the flag-clear and the op_rc-clear re-armed it for an op
 * that had already finished unwinding.  Either way the result was the same
 * silent failure: a LATER, unrelated BEGIN/FINISH got cancelled behind a
 * CC3501E_HW_OK the host never expected to mean "ignored".
 *
 * Fixed with a generation counter instead of a flag: ota_submit() bumps
 * ota_op_gen for every NEW op; cc3501e_hw_ota_abort() -- only when it observes
 * an op genuinely in flight -- records the generation IT SAW into
 * ota_abort_gen.  Everywhere that used to check `abort_requested` now checks
 * "does the CURRENT generation match the one an abort targeted" instead.  A
 * request that arrives too late to affect the op it targeted (case (a) above)
 * still remains STAMPED with that op's now-superseded generation, so it can
 * never falsely match a LATER op's generation -- no clear is needed for
 * correctness, unlike a boolean.  ota_submit()/cc3501e_hw_ota_abort() are both
 * only ever called from the single-threaded SPI dispatch context (see
 * cc3501e_hw_ota_write()'s header), so they never race each other -- only
 * pump() (the bring-up task) races either of them, which is exactly what these
 * two fields need to survive.  __atomic_* (not plain `volatile`) because this
 * is a genuine read-modify-decide handoff across that ISR/task seam, not just
 * a single flag flip. */
static volatile uint32_t ota_op_gen; /* current op's generation; 0 before the first ever op */
static volatile uint32_t
    ota_abort_gen; /* generation cc3501e_hw_ota_abort() last targeted; 0 = none yet */

static bool ota_this_op_aborted(void)
{
	return __atomic_load_n(&ota_abort_gen, __ATOMIC_ACQUIRE) ==
	       __atomic_load_n(&ota_op_gen, __ATOMIC_ACQUIRE);
}

/* Enqueue op @o (args already staged) and return BUSY: an op is in flight while
 * op_rc == OTA_OP_INFLIGHT.  The pump publishes the result + frees the slot
 * (auto-resets op to IDLE); the host observes completion through OTA_STATUS
 * (state / cursor), NOT by re-collecting -- so a WRITE poll never has to re-send
 * its 256 B payload while the device is mid-flash (which would disrupt the
 * phased bridge during the flash blackout).  Fast + ISR-safe (no flash here). */
static int ota_submit(uint8_t o)
{
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY; /* an op is running */
	__atomic_add_fetch(
	    &ota_op_gen, 1u, __ATOMIC_RELEASE); /* new generation before INFLIGHT is visible */
	ota.op    = o;
	ota.op_rc = OTA_OP_INFLIGHT;
	return CC3501E_HW_BUSY;
}

/* ---- slow bodies (run ONLY from ota_pump, off the SPI ISR) ----------------- */

/* Walk the target slot back to READY regardless of which PSA-FWU state an
 * abort caught it in -- WRITING/CANDIDATE from a partial FINISH, or STAGED
 * from one that reached psa_fwu_install() before the cancel landed.  This is
 * the exact 3-call recovery ota_do_begin() already runs for ANY stuck state
 * (cancel/reject/clean each no-op when the slot isn't in the matching state),
 * factored out so it can also run from cc3501e_hw_ota_abort() and
 * ota_finish_step() -- an aborted session must never leave a promotable image
 * behind (issue #1123 blocker: the first round of this fix skipped this and
 * only gated the two RAM writes, leaving the flash-committed image armed). */
static void ota_release_slot(uint8_t slot)
{
	(void)cc3501e_ota_psa_cancel(slot);
	(void)cc3501e_ota_psa_reject();
	(void)cc3501e_ota_psa_clean(slot);
}

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
		ota_release_slot(CC3501E_OTA_PSA_SLOT_1);
		ota_release_slot(CC3501E_OTA_PSA_SLOT_2);
		if (cc3501e_ota_psa_query_primary(CC3501E_OTA_PSA_SLOT_2, &primary2) && primary2) {
			target = CC3501E_OTA_PSA_SLOT_1;
		} else {
			target = CC3501E_OTA_PSA_SLOT_2;
		}
	}
	bool target_is_primary =
	    false; /* discarded -- this call is a fail-fast reachability check only */
	if (!cc3501e_ota_psa_query_primary(target, &target_is_primary)) return CC3501E_HW_ERR_IO;
	(void)target_is_primary;
	/* Walk ANY stuck state back to READY so a fresh stage always succeeds -- the
	 * common jam is a prior STAGED image the swap-reboot never promoted (e.g. a
	 * downgrade BL2 refused).  Mirrors ota_finish_step's START phase and the abort
	 * walk-back above; this lets a new (forward) OTA replace a stuck pending image
	 * instead of returning BAD_STATE forever. */
	ota_release_slot(target);
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
 * Called from cc3501e_hw_ota_pump() (issue #1123): returns CC3501E_HW_BUSY
 * while more of the sequence remains (pump() re-invokes on a later call),
 * CC3501E_HW_OK once STAGED + the reboot latch are armed, or an error.
 * Re-checks ota_this_op_aborted() at the top of every call (covers an abort
 * landing during START or WRITE -- the slot there is at most WRITING, not
 * yet a candidate image, so ota_do_begin()'s existing walk-back on the NEXT
 * session is sufficient and no immediate release is needed), again between
 * psa_fwu_finish() and psa_fwu_install() (the slot IS a candidate image
 * here -- CANDIDATE, still fully unwindable -- so THIS checkpoint calls
 * ota_release_slot() before returning), and a third time immediately after
 * psa_fwu_install() succeeds (STAGED -- same walk-back).  The one call this
 * can never wrap a check around is psa_fwu_install() itself (a single
 * vendor call, not decomposable further); even there, the walk-back
 * immediately after means an abort landing during that call still leaves
 * the slot at READY, not STAGED -- there is no longer any window where a
 * cancelled session can end up promotable. */
static int ota_finish_step(void)
{
	if (ota_this_op_aborted()) {
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
		 * still read READY, so this must NOT be gated on it (silicon 2026-06-19).
		 * (STAGED is the common stuck case here: a finish reached psa_fwu_install but
		 * the cold swap-reboot could not complete -- see project-cc3501e-firmware-bringup.) */
		ota_release_slot(ota.target);
		if (!cc3501e_ota_psa_start(ota.target, ota.image_buf, manifest_len)) {
			return CC3501E_HW_ERR_IO;
		}
		ota.finish_off   = manifest_len;
		ota.finish_phase = OTA_FINISH_PHASE_WRITE;
		return CC3501E_HW_BUSY;
	}

	case OTA_FINISH_PHASE_WRITE: {
		uint32_t n = ota.total_len - ota.finish_off; /* > 0: guaranteed by START's total_len check
		                                               * and by only ever re-entering WRITE below
		                                               * finish_off < total_len */
		if (n > CC3501E_OTA_FINISH_FLASH_BLOCK) {
			n = CC3501E_OTA_FINISH_FLASH_BLOCK;
		}
		if (!cc3501e_ota_psa_write(ota.target, ota.finish_off, &ota.image_buf[ota.finish_off], n)) {
			return CC3501E_HW_ERR_IO;
		}
		ota.finish_off += n;
		if (ota.finish_off >= ota.total_len) {
			ota.finish_phase = OTA_FINISH_PHASE_INSTALL; /* fold the transition into this
			                                               * step instead of burning an
			                                               * extra tick just to flip it */
		}
		return CC3501E_HW_BUSY; /* more blocks (or INSTALL) remain */
	}

	case OTA_FINISH_PHASE_INSTALL: {
		if (!cc3501e_ota_psa_finish(ota.target)) {
			return CC3501E_HW_ERR_IO;
		}
		if (ota_this_op_aborted()) {
			/* Slot is CANDIDATE here (finish() ran, install() has not) --
			 * still fully unwindable, same as the START-phase walk-back. */
			ota_release_slot(ota.target);
			return CC3501E_HW_ERR_STATE;
		}
		/* psa_fwu_install stages the swap (CANDIDATE -> STAGED); TI's
		 * PSA_SUCCESS_REBOOT return ("reboot to complete the swap") is folded into
		 * cc3501e_ota_psa_install()'s bool -- see cc3501e_hw_ti_ota_psa.c. */
		if (!cc3501e_ota_psa_install()) {
			return CC3501E_HW_ERR_IO;
		}
		if (ota_this_op_aborted()) {
			/* Landed during psa_fwu_install() itself -- the one vendor call this
			 * loop can't wrap a check around.  The image is now committed
			 * (CANDIDATE -> STAGED); walk it straight back to READY (reject
			 * before clean, same order as everywhere else) so a cancelled
			 * session never leaves a promotable image in the slot (#1123). */
			ota_release_slot(ota.target);
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
 * "Called once per tick" is a convenience label, not a scheduling guarantee:
 * worker_run_pending() (main.c) can itself block for seconds on a radio op
 * before cc3501e_hw_tick() (and so this pump) runs again, so a FINISH can now
 * sit mid-sequence -- a half-written slot -- for an unbounded stretch.  That
 * is not a new timeout risk (the host's own poll-by-repeat budget already has
 * to tolerate BUSY for however long FINISH takes), just a state this pre-#1123
 * single-shot ota_do_finish() could never be caught in.
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
		/* FINISH still chunking (ota_finish_step): stay INFLIGHT so a later call
		 * resumes where it left off, and so cc3501e_hw_ota_abort() gets a fresh
		 * checkpoint between every block instead of racing one monolithic burst. */
		return;
	}
	if (ota_this_op_aborted()) {
		/* Cancelled while BEGIN/FINISH was in flight -- cc3501e_hw_ota_abort()
		 * deferred the state clear to us instead of racing this publish.  Any
		 * flash the FINISH path committed was already walked back inside
		 * ota_finish_step() itself; this just resets the RAM session so it
		 * doesn't surface as WRITING/STAGED/ERROR from a run it asked to cancel,
		 * and does not touch ota_reboot_pending (ota_finish_step never set it on
		 * this path -- see its INSTALL-phase abort checks). */
		ota.finish_phase = OTA_FINISH_PHASE_START;
		ota.finish_off   = 0u;
		ota.state        = ALP_CC3501E_OTA_STATE_IDLE;
		ota.cursor       = 0u;
		ota.total_len    = 0u;
		ota.op           = OTA_OP_IDLE;
		ota.op_rc        = 0;
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
 * that lands after an abort HAS ALREADY BEEN PUBLISHED sees ota.state !=
 * WRITING and is rejected below, same as any WRITE without an open session.
 * But a DEFERRED abort (BEGIN/FINISH was mid-flight) does not flip
 * ota.state until cc3501e_hw_ota_pump() next runs -- ota.state is still
 * WRITING until then, so a WRITE that lands in that window is accepted (and,
 * for an already-staged byte range, replies CC3501E_HW_OK without touching
 * memory) even though the session is on its way to being cancelled. */
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
		 * context) -- do NOT stomp its state out from under it: the pre-#1123
		 * unconditional clear here raced ota_do_finish()'s STAGED/reboot_pending
		 * publish and could let an "aborted" update install and reboot anyway.
		 * Record the generation this call observed in flight; pump()/
		 * ota_finish_step() compare it against the CURRENT generation at each of
		 * their own checkpoints (ota_this_op_aborted()) rather than trusting a
		 * flag this call could have set a moment too late to matter -- see the
		 * ota_op_gen/ota_abort_gen comment above for why that closes the
		 * round-2 leak a plain bool had. */
		__atomic_store_n(
		    &ota_abort_gen, __atomic_load_n(&ota_op_gen, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
		return CC3501E_HW_OK;
	}
	/* Nothing in flight.  BEGIN/WRITE never touch the target slot's persistent
	 * flash state, so no walk-back is owed for those.  But a FINISH may have
	 * already completed (state == STAGED, not racing THIS call at all) and
	 * armed the swap-reboot -- issue #1123's blocker: an abort must never
	 * leave a promotable image behind, whether it raced the FINISH or simply
	 * arrived after it.  BRINGUP_STATUS.md documented this exact bench gap
	 * ("OTA_ABORT ... does not clear a committed STAGED image") before this
	 * fix; walk the slot back here the same way ota_finish_step() now does for
	 * the racing case, and disarm the latch that FINISH's own (unraced)
	 * success already set (ota_finish_step never gets a chance to gate this
	 * one -- it already published OK before this call ran). */
	if (ota.state == ALP_CC3501E_OTA_STATE_STAGED) {
		ota_release_slot(ota.target);
		ota_reboot_pending = false;
	}
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
	 * clean no-op.
	 *
	 * Deliberately left ungated on ota.state (issue #1123 review): gating this on
	 * state == STAGED would defeat the one scenario it exists for -- a bare reset
	 * that wipes ota.state to IDLE while flash still legitimately holds a
	 * committed image.  What matters is that a CANCELLED session can no longer
	 * leave a promotable image behind in the first place: every path that could
	 * reach psa_fwu_install() now also walks the slot back (ota_release_slot())
	 * whenever the caller asked to cancel -- see cc3501e_hw_ota_abort() and
	 * ota_finish_step()'s INSTALL phase.  So by the time PROMOTE can be reached
	 * with something genuinely pending, that pending image was never cancelled;
	 * PROMOTE booting it is the intended recovery, not the #1123 bug. */
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
