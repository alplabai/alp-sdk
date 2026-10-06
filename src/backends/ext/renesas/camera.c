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
 * After the gate the calls validate their arguments and return
 * ALP_ERR_NOSUPPORT: no ISP register is written and nothing is latched.
 * The CM33 FSP has no CRU/CSI-2/ISP module and the A55 owns them
 * (RZ/V2N Hardware User's Manual R01UH1071EJ0120 section 9.8).
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
	if (_state(camera) == NULL) return ALP_ERR_NOT_READY;

	return ALP_ERR_NOSUPPORT;
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
     * the SDK's accepted length range (16..1024). */
	if ((int)channel < 0 || (int)channel >= (int)ALP_V2N_N44_ISP_CHANNEL_COUNT) {
		return ALP_ERR_INVAL;
	}
	if (len < 16u || len > 1024u) {
		return ALP_ERR_INVAL;
	}
	if (_state(camera) == NULL) return ALP_ERR_NOT_READY;

	return ALP_ERR_NOSUPPORT;
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
	/* SDK-chosen bounds: 64 cells (8x8 grid) up to 4096. */
	if (len < 64u || len > 4096u) {
		return ALP_ERR_INVAL;
	}
	if (_state(camera) == NULL) return ALP_ERR_NOT_READY;

	return ALP_ERR_NOSUPPORT;
}
