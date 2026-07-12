/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E power-management policy host helper (0x62).  See
 * <alp/chips/cc3501e/power.h> for the public API.
 */

#include <stdint.h>

#include "cc3501e_internal.h"

alp_status_t
cc3501e_power_policy(cc3501e_t *ctx, const alp_cc3501e_power_policy_t *policy, uint32_t timeout_ms)
{
	if (policy == NULL) return ALP_ERR_INVAL;
	/* Pack the 8-byte wire by hand (NOT via the doc struct, which carries u16/u32
	 * alignment padding the wire does not) -- byte-for-byte what handle_power_policy
	 * parses: policy(1) | wake_events(1) | reserved(2,=0) | idle_ms_before_sleep(LE32). */
	const uint32_t idle = policy->idle_ms_before_sleep;
	uint8_t        req[8];
	req[0] = policy->policy;
	req[1] = policy->wake_events;
	req[2] = 0u;
	req[3] = 0u;
	req[4] = (uint8_t)(idle & 0xFFu);
	req[5] = (uint8_t)((idle >> 8) & 0xFFu);
	req[6] = (uint8_t)((idle >> 16) & 0xFFu);
	req[7] = (uint8_t)((idle >> 24) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_POWER_POLICY, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}
