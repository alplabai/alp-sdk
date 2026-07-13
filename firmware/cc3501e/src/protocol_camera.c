/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: camera-enable command-family handlers
 * (0x60..0x61).  Split out of protocol.c (issue #461); protocol_dispatch()
 * in protocol.c still owns the single command-family switch that routes
 * here.
 */

#include "protocol_internal.h"

/* CAM_ENABLE (0x60) / CAM_DISABLE (0x61): drive CAM_EN_LDO0/1.  Request
 * payload = 1 byte (which: 0 -> LDO0, 1 -> LDO1). */
alp_cc3501e_resp_t handle_cam_enable(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 1u));
}

alp_cc3501e_resp_t handle_cam_disable(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 0u));
}
