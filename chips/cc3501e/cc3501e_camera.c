/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E camera-enable LDO host helper (0x60/0x61).  See
 * <alp/chips/cc3501e/camera.h> for the public API.
 *
 * Synchronous + fast in the firmware (no worker, no radio bring-up), so
 * it takes the caller's timeout with no down-window floor.  poll_by_repeat
 * still absorbs a transient ALP_ERR_IO if a radio op happens to overlap
 * (the bridge is briefly down then).
 */

#include <stdint.h>
#include <stdbool.h>

#include "cc3501e_internal.h"

alp_status_t cc3501e_cam_enable(cc3501e_t *ctx, uint8_t which, bool on, uint32_t timeout_ms)
{
	uint8_t           req = which;
	alp_cc3501e_cmd_t cmd = on ? ALP_CC3501E_CMD_CAM_ENABLE : ALP_CC3501E_CMD_CAM_DISABLE;
	return poll_by_repeat(ctx, cmd, &req, sizeof(req), NULL, 0, NULL, timeout_ms);
}
