/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Coverage for the two legs of alp_ble_gatt_notify()'s conn-side gate
 * (src/ble_dispatch.c) that issue #1646 cluster 1 touched: neither a
 * NULL @p conn nor an op_enter failure on a real, since-disconnected
 * @p conn was previously exercised by any test (tests/zephyr/ble only
 * drives the h == NULL early-out).
 *
 * native_sim ships no real BLE controller (see
 * src/backends/ble/zephyr_drv.c's file header), so this suite never
 * calls bt_enable() -- instead it registers its OWN minimal ble
 * backend at priority 200 (ahead of zephyr_drv's 100), which is
 * enough to drive alp_ble_open()/alp_ble_connect()/alp_ble_disconnect()
 * through the REAL src/ble_dispatch.c (linked unconditionally into
 * every Zephyr build -- see zephyr/CMakeLists.txt) and reach a real,
 * pool-allocated alp_ble_conn_t. Kept in its own test binary (same
 * one-suite-per-fixture pattern as tests/zephyr/ble_gatt_server) so
 * this always-succeeds fake backend never leaks into tests/zephyr/ble's
 * "no controller -> NOSUPPORT" smoke suite.
 */

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/ble/ble_ops.h"

ZTEST_SUITE(alp_ble_gatt_notify_op_enter, NULL, NULL, NULL, NULL, NULL);

static alp_status_t fake_open(alp_ble_radio_state_t *state, alp_capabilities_t *caps_out)
{
	(void)state;
	(void)caps_out;
	return ALP_OK;
}

static alp_status_t fake_connect(alp_ble_radio_state_t *state,
                                 const alp_ble_addr_t  *peer,
                                 uint32_t               timeout_ms,
                                 alp_ble_conn_state_t  *conn_state_out)
{
	(void)state;
	(void)peer;
	(void)timeout_ms;
	(void)conn_state_out;
	return ALP_OK;
}

static alp_status_t fake_disconnect(alp_ble_conn_state_t *conn_state)
{
	(void)conn_state;
	return ALP_OK;
}

static alp_status_t fake_gatt_notify(alp_ble_radio_state_t *radio_state,
                                     alp_ble_conn_state_t  *conn_state,
                                     alp_ble_attr_handle_t  handle,
                                     const uint8_t         *payload,
                                     size_t                 len)
{
	(void)radio_state;
	(void)conn_state;
	(void)handle;
	(void)payload;
	(void)len;
	return ALP_OK;
}

static const alp_ble_ops_t _fake_ops = {
	.open        = fake_open,
	.connect     = fake_connect,
	.disconnect  = fake_disconnect,
	.gatt_notify = fake_gatt_notify,
};

ALP_BACKEND_REGISTER(ble,
                     faketest,
                     {
                         .silicon_ref = "*",
                         .vendor      = "faketest",
                         .base_caps   = 0u,
                         .priority    = 200,
                         .ops         = &_fake_ops,
                         .probe       = NULL,
                     });

ZTEST(alp_ble_gatt_notify_op_enter, test_notify_null_conn_returns_not_ready)
{
	alp_ble_t *h = alp_ble_open();
	zassert_not_null(h, "fake backend must win open() at priority 200");

	zassert_equal(alp_ble_gatt_notify(h, NULL, 0, NULL, 0),
	              ALP_ERR_NOT_READY,
	              "conn == NULL must report NOT_READY, matching the op_enter-fail leg");

	alp_ble_close(h);
}

ZTEST(alp_ble_gatt_notify_op_enter, test_notify_after_disconnect_returns_not_ready)
{
	alp_ble_t      *h    = alp_ble_open();
	alp_ble_addr_t  peer = { 0 };
	alp_ble_conn_t *conn = NULL;

	zassert_not_null(h, "fake backend must win open() at priority 200");
	zassert_equal(alp_ble_connect(h, &peer, 0, &conn), ALP_OK, "fake connect must succeed");
	zassert_not_null(conn, "connect must yield a real, pool-allocated conn");

	zassert_equal(alp_ble_disconnect(conn), ALP_OK, "fake disconnect must succeed");

	/* conn is now the closed/recycled slot -- op_enter's lifecycle check
	 * must fail, same as any other op_enter site (issue #1646). Before
	 * cluster 1's fix this returned ALP_ERR_INVAL, indistinguishable
	 * from a genuine caller bug. */
	zassert_equal(alp_ble_gatt_notify(h, conn, 0, NULL, 0),
	              ALP_ERR_NOT_READY,
	              "notify on a disconnected conn must report NOT_READY, not INVAL");

	alp_ble_close(h);
}
