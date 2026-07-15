/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/camera.h>.  VeriSilicon ISP Pico
 * (vsi,isp-pico) vendor-extension surface for the E8 backend
 * currently registered in alp-sdk.
 *
 * Two independent blockers here -- do not collapse them into one story.
 * AE / AF / LSC are pack-blocked: the Zephyr driver
 * (zephyr/drivers/video/isp_pico.c) already flags a HAL_ALIF VERSION
 * MISMATCH -- the locally vendored wrapper (modules/hal/alif/drivers/
 * isp/isp_wrapper) is a 2025 subset the driver compiles against but
 * cannot link.  The per-channel gain path fails for a different
 * reason -- it is contract-absent (see the entry below) and stays
 * NOSUPPORT even once the wrapper version is fixed.  No sensor is
 * wired on this SoM batch (#226) either, so nothing here can be
 * bench-verified regardless of the above.  Every body returns
 * ALP_ERR_NOSUPPORT after the standard vendor-handle gating, per the
 * Slice 6 storage.c precedents.
 *
 * Trap for whoever wires these (#223): the archive is a SUBSET of
 * inc/lib -- a declaration does not imply a linkable call, and a
 * linkable call does not imply it can satisfy the public contract
 * (see the gain entry below).  Check `nm -g --defined-only
 * libisp_gcc.a` first.  Per-entry state below.  E4 / E6 need explicit
 * backend registrations plus board validation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/camera.h>
#include <alp/peripheral.h>

#include "../../camera/camera_ops.h"

static bool _is_alif_backend(const alp_camera_t *c)
{
	return c != NULL && c->backend != NULL && c->backend->vendor != NULL &&
	       strcmp(c->backend->vendor, "alif") == 0;
}

alp_status_t alp_alif_camera_isp_3a_window_set(alp_camera_t                 *camera,
                                               alp_alif_camera_3a_region_t   region,
                                               const alp_alif_camera_rect_t *rect)
{
	if (camera == NULL || rect == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_alif_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if (rect->w == 0u || rect->h == 0u) {
		return ALP_ERR_INVAL;
	}
	(void)region;
	/* PARTIAL: of the three 3A regions, only AWB is reachable
	 * (ISP_WBM_ATTR_S.measRect via SetWbmAttr).  AE and AF are both
	 * dead, but for DIFFERENT reasons -- keep them distinct:
	 *   - AE: declared-but-undefined.  mpi_isp_expm.h:103 declares
	 *     Get/SetExpmAttr and mpi_isp_calib.h:47 inlines a call to it,
	 *     but the archive defines no Expm symbol at all.
	 *   - AF: absent outright.  No header under isp_wrapper/inc and no
	 *     symbol in the archive -- not even a declaration.
	 * Alif shipping Expm would fix AE only; AF would still be dead.
	 * Moving AWB alone while AE/AF stay silently unset is worse than
	 * NOSUPPORT, so this waits on both landing together. */
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_alif_camera_isp_gain_table_load(alp_camera_t             *camera,
                                                 alp_alif_camera_channel_t channel,
                                                 const uint16_t           *table,
                                                 uint16_t                  len)
{
	if (camera == NULL || table == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_alif_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if (len < 16u || len > 1024u) {
		return ALP_ERR_INVAL;
	}
	(void)channel;
	/* ABSENT: ISP_WB_ATTR_S.manualAttr.wbGain (opType = manual) via
	 * SetWbAttr is defined in the archive, but ISP_WB_GAIN_S is four
	 * by-value scalars (rGain/grGain/gbGain/bGain, range [256,1023]) --
	 * wrong cardinality (4 vs this call's 16..1024-entry table), wrong
	 * scale (not Q4.12), and wrong ownership (by-value vs the
	 * DMA-fetched by-reference LUT this contract promises).  A symbol
	 * existing is not the same as a symbol that can satisfy the
	 * contract.  Stays NOSUPPORT until Alif exposes an actual
	 * by-reference gain-table MPI call; the #226 sensor-wiring gap is
	 * orthogonal and doesn't change this. */
	return ALP_ERR_NOSUPPORT;
}

alp_status_t
alp_alif_camera_isp_lsc_lut_load(alp_camera_t *camera, const uint16_t *lut, uint16_t len)
{
	if (camera == NULL || lut == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!_is_alif_backend(camera)) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if (len < 64u || len > 4096u) {
		return ALP_ERR_INVAL;
	}
	/* ABSENT: no LSC header under isp_wrapper/inc and no LSC symbol in
	 * the archive -- not even a declaration.  Stays NOSUPPORT until Alif
	 * exposes an LSC MPI call. */
	return ALP_ERR_NOSUPPORT;
}
