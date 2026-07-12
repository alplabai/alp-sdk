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

#include <stdint.h>
#include <string.h> /* memcpy (OTA manifest buffering) */

#include <ti/utils/FWU/psa_fwu.h> /* PSA Firmware Update: stream + install the vendor image */

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* reply_drained / ota_reboot_pending / ota_reboot_rc */
#include "transport.h"              /* bridge_transport_spi_hw_reinit */

/* ===================================================================== */
/* OTA firmware update (over-the-bridge PSA-FWU streaming) -- v0.2.       */
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
 * transfer stays clean).  ALL the flash happens ONCE at FINISH (psa_fwu_start +
 * write the whole staged image + install), deferred to cc3501e_hw_ota_pump() on
 * the bring-up task with a single bridge re-arm after.  HOST_IRQ/async-event work
 * can revisit per-chunk flash + a smaller buffer.) */
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

static struct {
	uint8_t             state; /* alp_cc3501e_ota_state_t */
	psa_fwu_component_t target;
	uint32_t            total_len;
	uint32_t            cursor; /* bytes staged into image_buf so far */
	/* Deferred BEGIN/FINISH queue (ISR enqueues; ota_pump runs the flash). */
	volatile uint8_t op;       /* OTA_OP_* currently queued/running */
	volatile int8_t  op_rc;    /* OTA_OP_INFLIGHT while pending; else result */
	uint32_t         op_total; /* BEGIN arg */
	uint8_t          image_buf[CC3501E_OTA_IMAGE_MAX]; /* full image staged in RAM */
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
	(void)ti;
	(void)psa_fwu_cancel(target);                  /* WRITING/CANDIDATE -> FAILED */
	(void)psa_fwu_reject(PSA_ERROR_GENERIC_ERROR); /* STAGED           -> FAILED */
	(void)psa_fwu_clean(target);                   /* FAILED/UPDATED   -> READY  */
	ota.target    = target;
	ota.total_len = ota.op_total;
	ota.cursor    = 0u;
	ota.state     = ALP_CC3501E_OTA_STATE_WRITING;
	return CC3501E_HW_OK;
}

/* FINISH: commit the whole RAM-staged image to the target slot in ONE flash burst
 * (manifest = first TI_FWU_MANIFEST_SIZE bytes -> psa_fwu_start; the remainder in
 * CC3501E_OTA_FINISH_FLASH_BLOCK pages -> psa_fwu_write), finalize + install, then arm
 * the swap-reboot.  All the OTA flash (hence all bridge-DMA disruption) is here. */
static int ota_do_finish(void)
{
	if (ota.cursor != ota.total_len || ota.total_len <= (uint32_t)TI_FWU_MANIFEST_SIZE) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* Force the target component's persistent flash flow-state to READY before
	 * psa_fwu_start.  A prior failed/partial OTA leaves the flash flow-state stuck
	 * (set inside psa_fwu_start / _install), and psa_fwu_start's own flow_check then
	 * returns PSA_ERROR_BAD_STATE(-137) forever -- the RAM ComponentInfo.state can
	 * still read READY, so this must NOT be gated on it (silicon 2026-06-19).  Walk
	 * every stuck state back to READY (ignore each rc -- they no-op when N/A):
	 *   cancel  WRITING/CANDIDATE -> FAILED
	 *   reject  STAGED            -> FAILED   (an install that never swap-booted)
	 *   clean   FAILED/UPDATED    -> READY
	 * (STAGED is the common stuck case here: a finish reached psa_fwu_install but
	 * the cold swap-reboot could not complete -- see project-cc3501e-firmware-bringup.) */
	(void)psa_fwu_cancel(ota.target);
	(void)psa_fwu_reject(PSA_ERROR_GENERIC_ERROR);
	(void)psa_fwu_clean(ota.target);

	if (psa_fwu_start(ota.target, ota.image_buf, TI_FWU_MANIFEST_SIZE) != PSA_SUCCESS) {
		return CC3501E_HW_ERR_IO;
	}
	uint32_t since_rearm = 0u;
	for (uint32_t off = (uint32_t)TI_FWU_MANIFEST_SIZE; off < ota.total_len;) {
		uint32_t n = ota.total_len - off;
		if (n > CC3501E_OTA_FINISH_FLASH_BLOCK) {
			n = CC3501E_OTA_FINISH_FLASH_BLOCK;
		}
		if (psa_fwu_write(ota.target, off, &ota.image_buf[off], n) != PSA_SUCCESS) {
			return CC3501E_HW_ERR_IO;
		}
		off += n;
		/* Re-arm the bridge slave periodically across the flash burst so the host's
		 * header-only FINISH poll keeps getting serviced (BUSY) instead of a long IO
		 * blackout that would time out the host (silicon 2026-06-19). */
		if (++since_rearm >= 2u) {
			since_rearm = 0u;
			bridge_transport_spi_hw_reinit();
		}
	}
	psa_status_t pf = psa_fwu_finish(ota.target);
	if (pf != PSA_SUCCESS && pf != PSA_SUCCESS_REBOOT) {
		return CC3501E_HW_ERR_IO;
	}
	/* psa_fwu_install stages the swap and returns PSA_SUCCESS_REBOOT(1) -- a SUCCESS
	 * code meaning "reboot to complete the swap", NOT an error. */
	psa_status_t pi = psa_fwu_install(); /* CANDIDATE -> STAGED */
	if (pi != PSA_SUCCESS && pi != PSA_SUCCESS_REBOOT) {
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
 * header-only STATUS, so nothing is half-served across the flash). */
void cc3501e_hw_ota_pump(void)
{
	if (ota.op_rc != OTA_OP_INFLIGHT) return; /* nothing queued */
	int rc;
	switch (ota.op) {
	case OTA_OP_BEGIN:
		rc = ota_do_begin();
		break;
	case OTA_OP_FINISH:
		rc = ota_do_finish();
		break;
	default:
		rc = CC3501E_HW_ERR_INVAL;
		break;
	}
	if (rc != CC3501E_HW_OK && rc != CC3501E_HW_BUSY) {
		ota.state = ALP_CC3501E_OTA_STATE_ERROR;
	}
	bridge_transport_spi_hw_reinit(); /* flash tore the bridge DMA down -- re-open + re-arm */
	ota.op    = OTA_OP_IDLE;          /* free the slot -- result is observable via STATUS */
	ota.op_rc = (int8_t)rc;           /* publish LAST: clears INFLIGHT so a new op can queue */
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
 * on the cursor so a host re-send of an already-staged chunk is harmless. */
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
	return ota_submit(OTA_OP_FINISH);
}

int cc3501e_hw_ota_abort(void)
{
	/* Discard the RAM-staged image.  No psa_fwu_cancel needed: FINISH is the only
	 * thing that touches the slot, so an aborted session never opened one. */
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
