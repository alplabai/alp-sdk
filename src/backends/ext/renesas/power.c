/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas RZ/V2N power vendor-extension body.  Implements the
 * surface declared in <alp/ext/renesas/power.h> by routing through
 * the GD32G553 supervisor chip driver
 * (gd32g553_power_mode_set in chips/gd32g553/gd32g553.c:747).
 *
 * Vendor-handle gate (mirrors src/backends/adc/alif_e7.c
 * vendor-ext bodies):
 *   - NULL handle  -> ALP_ERR_INVAL.
 *   - non-Renesas  -> ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * The call holds the supervisor mutex around the wire transaction
 * via alp_z_v2n_supervisor_acquire / _release so it interleaves
 * cleanly with concurrent portable peripheral traffic on the
 * GD32 bridge (ADC reads, PWM updates, DAC writes).
 *
 * On builds without CONFIG_ALP_SDK_V2N_SUPERVISOR=y the
 * supervisor singleton's NOSUPPORT stubs at the bottom of
 * src/zephyr/v2n_supervisor.c surface naturally -- this TU
 * still compiles + links cleanly so apps that #include the
 * vendor-ext header keep linking even when the supervisor isn't
 * wired in.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/gd32g553.h>
#include <alp/ext/renesas/power.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "../../power/power_ops.h"
#include "../../../zephyr/v2n_supervisor.h"

alp_status_t
alp_renesas_power_supervisor_mode_set(alp_power_t                        *handle,
                                      alp_renesas_power_supervisor_mode_t supervisor_mode)
{
	if (handle == NULL) {
		return ALP_ERR_INVAL;
	}
	if (handle->backend == NULL || strcmp(handle->backend->vendor, "renesas") != 0) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}

	/* Acquire the supervisor under the shared mutex so the wire
     * transaction can't interleave with a concurrent portable
     * peripheral call.  Failed acquires return naturally without
     * a release (mirrors the contract in v2n_supervisor.h). */
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		return s;
	}

	/* Forward the mirrored wake-bitmap from the dispatcher; the
     * supervisor honours the bitmap on its wake-source enable
     * pass.  wake_after_ms is 0 for this low-level entry point --
     * callers wanting timed wake should use the portable
     * alp_power_request_sleep instead. */
	s = gd32g553_power_mode_set(ctx, supervisor_mode, handle->state.wake_bitmap,
	                            0u /* no timed wake from this path */);
	alp_z_v2n_supervisor_release();
	return s;
}
