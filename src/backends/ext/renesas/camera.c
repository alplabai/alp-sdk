/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/renesas/camera.h>.  Finer-grained ISP knobs
 * for the Renesas RZ/V2N N44 ISP block (3A windows, per-channel
 * gain tables, LSC LUT).
 *
 * Vendor-handle gate (mirrors src/backends/ext/alif/storage.c +
 * src/backends/ext/renesas/power.c):
 *   - NULL handle -> ALP_ERR_INVAL.
 *   - non-Renesas backend -> ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * After the gate the calls reach into the V2N N44 ISP backend's
 * per-handle state (alp_v2n_n44_isp_state_t -- shared via
 * src/backends/camera/v2n_n44_isp.h) and latch the requested
 * tuning.  The real MMIO writes happen when the V2N N44 Zephyr
 * SoC port grows the ISP control-register driver (Renesas
 * Hardware User's Manual r01uh1003ej §18 "Image Signal
 * Processor" -- TBD register addresses + bit layouts).
 *
 * Stub vs real split per the slice spec:
 *
 *   ALP_OK skeleton (latch + parameter validation, no MMIO yet):
 *     - alp_renesas_camera_isp_3a_window_set
 *     - alp_renesas_camera_isp_gain_table_load
 *     - alp_renesas_camera_isp_lsc_lut_load
 *
 *   These bodies validate, latch, and return ALP_OK so the
 *   surface is exercisable end-to-end against the v2n_n44_isp
 *   backend.  When the N44 ISP register surface lands in the
 *   Zephyr port, the latched state feeds directly into the
 *   matching MMIO writes at the comment marked
 *   "TBD: real MMIO" inside each function below.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/ext/renesas/camera.h>
#include <alp/peripheral.h>

#include "../../camera/camera_ops.h"
#include "../../camera/v2n_n44_isp.h"

static bool _is_renesas_backend(const alp_camera_t *c)
{
	return c != NULL && c->backend != NULL && c->backend->vendor != NULL &&
	       strcmp(c->backend->vendor, "renesas") == 0;
}

static alp_v2n_n44_isp_state_t *_state(alp_camera_t *c)
{
	return (alp_v2n_n44_isp_state_t *)c->state.be_data;
}

alp_status_t alp_renesas_camera_isp_3a_window_set(alp_camera_t                    *camera,
                                                  alp_renesas_camera_3a_region_t   region,
                                                  const alp_renesas_camera_rect_t *rect)
{
	if (camera == NULL || rect == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_renesas_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if (rect->w == 0u || rect->h == 0u) {
		return ALP_ERR_INVAL;
	}
	/* Enum range-check.  The header declares three named entries;
     * any out-of-range value is rejected as INVAL rather than
     * silently latched into an unused slot. */
	if ((int)region < 0 || (int)region >= (int)ALP_V2N_N44_ISP_3A_REGION_COUNT) {
		return ALP_ERR_INVAL;
	}
	alp_v2n_n44_isp_state_t *st = _state(camera);
	if (st == NULL) return ALP_ERR_NOT_READY;

	/* Map public-enum slot to the internal slot.  Order matches
     * the header's ALP_RENESAS_CAMERA_3A_* enumeration exactly. */
	size_t slot             = (size_t)region;
	st->region_3a[slot].x   = rect->x;
	st->region_3a[slot].y   = rect->y;
	st->region_3a[slot].w   = rect->w;
	st->region_3a[slot].h   = rect->h;
	st->region_3a_set[slot] = true;

	/* TBD: real MMIO -- write the rectangle into the matching
     * AE / AWB / AF window register pair on the V2N N44 ISP.
     * Datasheet r01uh1003ej §18.3 "3A statistics" carries the
     * register map; the addresses land with the Zephyr SoC port. */
	return ALP_OK;
}

alp_status_t alp_renesas_camera_isp_gain_table_load(alp_camera_t                *camera,
                                                    alp_renesas_camera_channel_t channel,
                                                    const uint16_t              *table,
                                                    uint16_t                     len)
{
	if (camera == NULL || table == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_renesas_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	/* Channel enum range-check + length range-check per
     * datasheet §18.5 "Per-channel gain LUT" (16..1024). */
	if ((int)channel < 0 || (int)channel >= (int)ALP_V2N_N44_ISP_CHANNEL_COUNT) {
		return ALP_ERR_INVAL;
	}
	if (len < 16u || len > 1024u) {
		return ALP_ERR_INVAL;
	}
	alp_v2n_n44_isp_state_t *st = _state(camera);
	if (st == NULL) return ALP_ERR_NOT_READY;

	/* Reference-not-copy -- caller owns the buffer lifetime per
     * the header contract.  Stashing the pointer keeps the hot
     * path free of allocator pressure for the typical
     * 256..1024-entry curves customers ship. */
	st->gain_table[channel]     = table;
	st->gain_table_len[channel] = len;

	/* TBD: real MMIO -- the gain LUT uploads via a DMA-backed
     * control descriptor on N44 silicon (datasheet §18.5).
     * Until the port lands the latched pointer is consumed only
     * by future test hooks (vendor-ext getter -- pending). */
	return ALP_OK;
}

alp_status_t
alp_renesas_camera_isp_lsc_lut_load(alp_camera_t *camera, const uint16_t *lut, uint16_t len)
{
	if (camera == NULL || lut == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_renesas_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	/* Datasheet §18.6 caps the LSC grid at 4096 cells; lower
     * bound is empirical (64 cells = 8x8 grid still useful for
     * lens-vignetting correction). */
	if (len < 64u || len > 4096u) {
		return ALP_ERR_INVAL;
	}
	alp_v2n_n44_isp_state_t *st = _state(camera);
	if (st == NULL) return ALP_ERR_NOT_READY;

	st->lsc_lut     = lut;
	st->lsc_lut_len = len;

	/* TBD: real MMIO -- LSC LUT upload via the ISP's SRAM bank
     * (datasheet §18.6).  Until the SoC port lands the latched
     * pointer + length stays in backend memory for the future
     * upload path. */
	return ALP_OK;
}
