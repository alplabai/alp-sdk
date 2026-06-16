/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/camera.h>.  Mali-C55 ISP fabric on
 * Ensemble E4 / E6 / E8.
 *
 * No SoM in scope ships the Alif Mali-C55 HAL pack yet, so every
 * function here returns ALP_ERR_NOSUPPORT after the standard
 * vendor-handle gating (NULL handle -> INVAL; non-Alif backend ->
 * NOT_PRESENT_ON_THIS_SOC).  Mirrors the OSPI SecAES + FlexSPI
 * OTFAD precedents from Slice 6
 * (src/backends/ext/alif/storage.c + src/backends/ext/nxp/storage.c).
 *
 * When the Alif HAL Mali-C55 pack lands, an alif_mali_c55_isp
 * camera backend will register at priority 100 against the
 * three supported silicon_refs (alif:ensemble:e4 / e6 / e8) and
 * the bodies below will dispatch through the new backend's
 * private state.  The header + the gating stay unchanged at
 * that point.
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
	/* Mali-C55 HAL pack not in scope yet -- body lands with the
     * Ensemble E4 / E6 / E8 vendor integration. */
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_alif_camera_isp_gain_table_load(alp_camera_t             *camera,
                                                 alp_alif_camera_channel_t channel,
                                                 const uint16_t *table, uint16_t len)
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
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_alif_camera_isp_lsc_lut_load(alp_camera_t *camera, const uint16_t *lut,
                                              uint16_t len)
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
	return ALP_ERR_NOSUPPORT;
}
