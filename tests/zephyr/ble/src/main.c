/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the <alp/ble.h> wrapper under native_sim.  No BT
 * controller present -- the wrapper falls back to NOSUPPORT and
 * we verify the public-API contract for that path plus every
 * NULL-arg branch.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/ble.h"

ZTEST_SUITE(alp_ble, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_ble, test_open_no_controller_returns_null)
{
	alp_ble_t *b = alp_ble_open();
	zassert_is_null(b, "alp_ble_open without BT must yield NULL");
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
	              (int)alp_last_error());
}

ZTEST(alp_ble, test_advertise_null_handle_errors)
{
	alp_ble_adv_config_t cfg = { .name = "alp" };
	zassert_equal(alp_ble_advertise_start(NULL, &cfg), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_advertise_stop(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_ble, test_scan_null_handle_errors)
{
	zassert_equal(alp_ble_scan_start(NULL, true, NULL, NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_scan_stop(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_ble, test_connect_null_handle_errors)
{
	alp_ble_addr_t  peer = { 0 };
	alp_ble_conn_t *out  = NULL;
	zassert_equal(alp_ble_connect(NULL, &peer, 1000, &out), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_disconnect(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_ble, test_gatt_null_handle_errors)
{
	/* NULL / closed handle reports NOT_READY before any backend
     * dispatch -- the standard wrapper convention (the chips smoke
     * suite documents the same NOSUPPORT->NOT_READY shift for
     * audio/ble/mproc).  "No controller" is surfaced at open() time:
     * alp_ble_open() returns NULL (see test_open_no_controller_returns_null). */
	zassert_equal(alp_ble_gatt_register_service(NULL, NULL, NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_gatt_notify(NULL, NULL, 0, NULL, 0), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_gatt_read(NULL, 0, NULL, 0, NULL, 100), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_gatt_write(NULL, 0, NULL, 0, 100), ALP_ERR_NOT_READY);
}

ZTEST(alp_ble, test_close_null_is_safe)
{
	alp_ble_close(NULL);
}
