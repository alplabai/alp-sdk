/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression suite for alp-sdk#1631: an FD-flagged frame sent on a handle
 * opened ALP_CAN_MODE_CLASSIC must be refused by the DISPATCHER, before any
 * backend sees it.
 *
 * The defect was a stack overflow, not a wrong answer.
 * src/backends/can/zephyr_drv.c copies frame->payload_len bytes into an
 * on-stack `struct can_frame`, whose data[] is CAN_MAX_DLEN -- 8 without
 * CONFIG_CAN_FD_MODE, 64 with it.  A {fd = true, payload_len = 64} frame passed
 * BOTH length arms in src/can_dispatch.c: 64 is not > ALP_CAN_MAX_PAYLOAD_BYTES_FD,
 * and the classic arm was skipped precisely because frame->fd was set.  The
 * backend then wrote 56 bytes past the destination.
 *
 * WHY THIS IS A SEPARATE SUITE, and why it uses the alp_testing double:
 *
 *   - sw_fallback (which tests/unit/can_registry drives) answers
 *     ALP_ERR_NOSUPPORT to EVERY send.  Asserting NOSUPPORT against it passes
 *     identically with the dispatcher guard present or removed -- a vacuous
 *     test.  alp_testing's t_send() returns ALP_OK and captures the frame, so
 *     refusal and delivery are distinguishable.
 *   - The alp_testing backend registers at priority 255 and outranks every
 *     real/proxy/fallback backend, so enabling it changes backend SELECTION
 *     for every other test linked into the same binary.  Putting it in
 *     can_registry's prj.conf broke four unrelated cases there.
 *
 * Validated the way a regression test has to be: with the guard reverted, the
 * tx-drain assertions below fail.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/can.h>
#include <alp/testing/can.h>

#include "../../../../src/backends/can/can_ops.h"
#include "../../../../src/common/alp_slot_claim.h"

extern const alp_backend_t __start_alp_backends_can[];
extern const alp_backend_t __stop_alp_backends_can[];

static const alp_can_ops_t *_find_testing_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_can; be < __stop_alp_backends_can; ++be) {
		if (strcmp(be->vendor, "alp_testing") == 0) {
			return (const alp_can_ops_t *)be->ops;
		}
	}
	return NULL;
}

/* Build a started CLASSIC handle bound to the capturing double.
 *
 * alp_can_open() would pick a backend by priority; this suite needs the double
 * specifically, so the handle is assembled the way can_registry's own tests do
 * it -- state first (CONTAINER_OF depends on that), then the fields
 * alp_can_open() stamps: ops, in_use, lifecycle (issue #629's op-vs-close
 * guard), and cfg, which is the config snapshot the new guard reads. */
static void _open_classic(struct alp_can *h, const alp_can_ops_t *ops)
{
	memset(h, 0, sizeof(*h));
	alp_capabilities_t     caps = { 0 };
	const alp_can_config_t cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	zassert_equal(ops->open(&cfg, &h->state, &caps), ALP_OK);
	h->state.ops = ops;
	h->in_use    = true;
	h->lifecycle = ALP_HANDLE_LC_OPEN;
	h->cfg       = cfg;
	h->started   = true;
}

ZTEST(alp_can_fd_guard, test_fd_frame_on_classic_handle_never_reaches_backend)
{
	const alp_can_ops_t *ops = _find_testing_ops();
	zassert_not_null(ops, "alp_testing CAN backend not linked into this build");

	struct alp_can h;
	_open_classic(&h, ops);

	alp_can_frame_t drained[4];
	/* Start from a known-empty ring: the double is create-on-first-touch and
     * shared per bus_id across cases in this binary. */
	(void)alp_testing_can_tx_drain(0u, drained, ARRAY_SIZE(drained));

	/* The exact frame that overflowed. */
	alp_can_frame_t fd_frame = {
		.id          = 0x123u,
		.payload_len = ALP_CAN_MAX_PAYLOAD_BYTES_FD,
		.fd          = true,
	};
	memset(fd_frame.data, 0xA5, sizeof(fd_frame.data));
	zassert_equal(alp_can_send(&h, &fd_frame, 0u), ALP_ERR_NOSUPPORT);

	/* Short FD frame: the defect is the mode contradiction, not the length.
     * This one never overflowed, but it did put FDF on the wire of a
     * controller opened classic. */
	fd_frame.payload_len = 8u;
	zassert_equal(alp_can_send(&h, &fd_frame, 0u), ALP_ERR_NOSUPPORT);

	/* THE load-bearing assertion.  With the dispatcher guard removed both
     * sends above return ALP_OK and land here. */
	zassert_equal(alp_testing_can_tx_drain(0u, drained, ARRAY_SIZE(drained)),
	              0u,
	              "an FD frame on a CLASSIC handle reached the backend");

	ops->close(&h.state);
}

ZTEST(alp_can_fd_guard, test_guard_does_not_break_ordinary_classic_traffic)
{
	const alp_can_ops_t *ops = _find_testing_ops();
	zassert_not_null(ops);

	struct alp_can h;
	_open_classic(&h, ops);

	alp_can_frame_t drained[4];
	(void)alp_testing_can_tx_drain(0u, drained, ARRAY_SIZE(drained));

	alp_can_frame_t classic = {
		.id          = 0x123u,
		.payload_len = ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC,
		.fd          = false,
	};
	memset(classic.data, 0x5Au, ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC);
	zassert_equal(alp_can_send(&h, &classic, 0u), ALP_OK);
	zassert_equal(alp_testing_can_tx_drain(0u, drained, ARRAY_SIZE(drained)), 1u);
	zassert_equal(drained[0].payload_len, ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC);

	/* The pre-existing over-length arm must still fire, and keep its OWN
     * error code -- the new guard must not have swallowed ALP_ERR_INVAL into
     * ALP_ERR_NOSUPPORT. */
	classic.payload_len = (uint8_t)(ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC + 1u);
	zassert_equal(alp_can_send(&h, &classic, 0u), ALP_ERR_INVAL);

	ops->close(&h.state);
}

ZTEST_SUITE(alp_can_fd_guard, NULL, NULL, NULL, NULL, NULL);
