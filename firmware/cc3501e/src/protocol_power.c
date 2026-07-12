/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: power-policy command-family handler (0x62).
 * Split out of protocol.c (issue #461); protocol_dispatch() in
 * protocol.c still owns the single command-family switch that routes
 * here.
 */

#include "protocol_internal.h"

/* POWER_POLICY (0x62): packed wire = policy(1) | wake_events(1) |
 * reserved(2) | idle_ms_before_sleep(LE32) = 8 bytes. */
alp_cc3501e_resp_t handle_power_policy(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 8u) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint8_t  policy      = req[0];
	const uint8_t  wake_events = req[1];
	const uint32_t idle_ms = (uint32_t)req[4] | ((uint32_t)req[5] << 8) | ((uint32_t)req[6] << 16) |
	                         ((uint32_t)req[7] << 24);
	return hw_to_resp(cc3501e_hw_set_power_policy(policy, wake_events, idle_ms));
}
