/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/camera.h>.  VeriSilicon ISP Pico
 * (vsi,isp-pico) vendor-extension surface for the E8 backend
 * currently registered in alp-sdk.
 *
 * hal_alif DOES ship the ISP Pico wrapper (modules/hal/alif
 * drivers/isp/isp_wrapper: libisp_gcc.a + the VeriSilicon
 * VSI_MPI_ISP_* attribute API).  These bodies are unwired, not
 * pack-blocked: no camera sensor is wired on the current SoM batch
 * (issue #226), so an ISP path cannot be runtime-verified, and
 * alp-sdk does not merge silicon paths it has not run on the bench.
 * Every function therefore returns ALP_ERR_NOSUPPORT after the
 * standard vendor-handle gating (NULL handle -> INVAL; non-Alif
 * backend -> NOT_PRESENT_ON_THIS_SOC).  Mirrors the OSPI SecAES +
 * FlexSPI OTFAD precedents from Slice 6
 * (src/backends/ext/alif/storage.c + src/backends/ext/nxp/storage.c).
 *
 * When a sensor reaches the bench, these bind onto the wrapper's MPI
 * calls through the alif_isp_pico backend's private state on
 * alif:ensemble:e8 (issue #223) -- but only where libisp_gcc.a
 * actually defines the setter.  The shipped archive is a SUBSET of
 * inc/lib: headers declare more of the VSI MPI surface than the blob
 * implements, so check `nm -g --defined-only libisp_gcc.a` before
 * assuming a call links.  Widening this surface to E4 / E6 requires
 * explicit backend registrations plus board validation.
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
	/* Backing API is PARTIAL, so this stays NOSUPPORT even once a
	 * sensor lands.  The AWB metering window is reachable
	 * (ISP_WBM_ATTR_S.measRect via VSI_MPI_ISP_SetWbmAttr(), defined in
	 * libisp_gcc.a).  The AE one is not: mpi_isp_expm.h declares
	 * Get/SetExpmAttr() and mpi_isp_calib.h even inlines a call to it,
	 * but the archive defines NO Expm symbol -- wiring it links-errors.
	 * A 3A window that silently moved AWB and not AE would be worse
	 * than NOSUPPORT, so this waits on Alif shipping Expm (#223). */
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
	/* Backing API is complete: per-channel gain is
	 * ISP_WB_ATTR_S.manualAttr.wbGain (opType = manual), uploaded via
	 * VSI_MPI_ISP_SetWbAttr() -- declared AND defined in libisp_gcc.a.
	 * Of the three entry points here this is the only one whose body is
	 * blocked purely on bench access, not on a missing symbol: it wires
	 * up once a sensor is on the bench (#226). */
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
	/* No backing API at all -- weaker than the 3A window, which at least
	 * has declarations: lens-shading has no LSC header under
	 * isp_wrapper/inc AND no LSC symbol in libisp_gcc.a.  Stays
	 * NOSUPPORT after a sensor lands, until Alif exposes an LSC MPI
	 * call (#223). */
	return ALP_ERR_NOSUPPORT;
}
