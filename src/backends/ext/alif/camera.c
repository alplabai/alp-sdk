/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/camera.h>.  VeriSilicon ISP Pico
 * (vsi,isp-pico) vendor-extension surface for the E8 backend
 * currently registered in alp-sdk.
 *
 * Unwired, not pack-blocked: hal_alif DOES ship the wrapper
 * (modules/hal/alif drivers/isp/isp_wrapper -- libisp_gcc.a + the
 * VSI_MPI_ISP_* API).  No sensor is wired on this SoM batch (#226), so
 * no ISP path can be bench-verified.  Every body returns
 * ALP_ERR_NOSUPPORT after the standard vendor-handle gating, per the
 * Slice 6 storage.c precedents.
 *
 * Trap for whoever wires these (#223): the archive is a SUBSET of
 * inc/lib -- a declaration does not imply a linkable call.  Check
 * `nm -g --defined-only libisp_gcc.a` first.  Per-entry state below.
 * E4 / E6 need explicit backend registrations plus board validation.
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
	/* PARTIAL: AWB window is reachable (ISP_WBM_ATTR_S.measRect via
	 * SetWbmAttr), AE is not -- mpi_isp_expm.h declares Get/SetExpmAttr
	 * and calib.h inlines a call, but the archive defines no Expm symbol
	 * at all.  Moving AWB while silently skipping AE is worse than
	 * NOSUPPORT, so this waits on Alif shipping Expm. */
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
	/* COMPLETE: ISP_WB_ATTR_S.manualAttr.wbGain (opType = manual) via
	 * SetWbAttr, defined in the archive.  Only entry point here blocked
	 * on bench access alone -- wires up once a sensor lands (#226). */
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
