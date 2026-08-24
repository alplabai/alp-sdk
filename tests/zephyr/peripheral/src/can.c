/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/can.h> -- CAN wrapper tests.  Extracted from main.c in §C.16.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/can.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_can_null_cfg)
{
	zassert_is_null(alp_can_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_can_zero_bitrate_rejected)
{
	alp_can_t *c = alp_can_open(&(alp_can_config_t){ .bus_id             = 0,
	                                                 .bitrate_nominal_hz = 0, /* INVAL */
	                                                 .mode               = ALP_CAN_MODE_CLASSIC });
	zassert_is_null(c);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

/* #1631: an fd-flagged, 64-byte frame sent on a handle opened
 * ALP_CAN_MODE_CLASSIC must be rejected cleanly -- src/can_dispatch.c's
 * payload_len bound check alone lets it through (64 is not
 * > ALP_CAN_MAX_PAYLOAD_BYTES_FD, and the classic-payload arm is
 * skipped because frame->fd is true), so without the mode guard this
 * reaches src/backends/can/zephyr_drv.c's z_send(), which memcpy()s
 * frame->payload_len bytes into an on-stack struct can_frame sized by
 * Zephyr's CAN_MAX_DLEN -- 8 bytes on this suite's build (no
 * CONFIG_CAN_FD_MODE) -- a 56-byte stack overflow. */
ZTEST(alp_peripheral, test_can_fd_frame_rejected_on_classic_handle)
{
	alp_can_t *c = alp_can_open(&(alp_can_config_t){
	    .bus_id = 0, .bitrate_nominal_hz = 500000, .mode = ALP_CAN_MODE_CLASSIC });
	zassert_not_null(c, "alp_can_open(0) returned NULL -- is native_sim's alp-can0 alias wired?");
	zassert_equal(alp_can_start(c), ALP_OK, "alp_can_start() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x123;
	frame.fd              = true;
	frame.payload_len     = ALP_CAN_MAX_PAYLOAD_BYTES_FD; /* 64 */
	memset(frame.data, 0x5A, sizeof(frame.data));

	alp_status_t rc = alp_can_send(c, &frame, 100);
	zassert_true(rc == ALP_ERR_NOSUPPORT || rc == ALP_ERR_INVAL,
	             "fd-flagged 64-byte send on a classic-mode handle returned %d, not a clean "
	             "rejection -- overflow risk in the backend's memcpy",
	             (int)rc);

	alp_can_close(c);
}
