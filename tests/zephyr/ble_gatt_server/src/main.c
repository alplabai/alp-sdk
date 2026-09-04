/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GATT server tests for the Zephyr <alp/ble.h> backend (issue #480):
 * runtime service registration + characteristic read/write.
 *
 * bt_gatt_service_register() is a pure host-stack call -- it needs
 * neither bt_enable() nor a live controller (see zephyr_drv.c's file
 * header, and upstream's own tests/bluetooth/gatt, which proves the
 * same thing).  native_sim ships no BLE controller reachable offline
 * (BT_USERCHAN needs a real, powered-off host Bluetooth adapter), so
 * this suite never calls alp_ble_open()/bt_enable() -- it reaches the
 * backend directly through the registry, exactly the way the
 * dispatcher (src/ble_dispatch.c) would, minus the handle-pool
 * bookkeeping.  Mirrors the pattern already established in
 * tests/unit/ble_registry (selector + public-API edges; "unit coverage
 * stops at provider selection" there).  This suite goes one step
 * further: it drives the SELECTED backend's real ops, proving the
 * register/read/write bodies -- not just that a backend is reachable.
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>

#include "alp/backend.h"
#include "alp/ble.h"
#include "alp/peripheral.h"
#include "alp/soc_caps.h"

#include "../../../../src/backends/ble/ble_ops.h"

/* White-box seam for the client-read callback (src/backends/ble/zephyr_drv.c,
 * CONFIG_ZTEST-only) -- see that file for why this is the smallest
 * offline-reproducible harness for the STOP-suppresses-completion bug. */
extern alp_status_t alp_ble_test_read_cb(uint8_t err, const void *data, uint16_t length);

/* White-box seams for the GATT read/write context pool's three branches
 * that changelog.d/1620.md records as unexecuted (issue #1939) --
 * native_sim has no BLE controller to drive a real peer through them, so
 * each one calls the real backend function directly with a synthetic
 * ctx. See src/backends/ble/zephyr_drv.c for the CONFIG_ZTEST
 * definitions. */
extern alp_status_t
alp_ble_test_read_cb_after_abandon(uint8_t err, const void *data, uint16_t length);
extern alp_status_t alp_ble_test_read_timeout(void);
extern alp_status_t alp_ble_test_write_cb_after_abandon(uint8_t err);
extern alp_status_t alp_ble_test_write_timeout(void);
extern void         alp_ble_test_set_ctx_pools_exhausted(bool exhausted);
extern void        *alp_ble_test_fake_conn_be(void);

ZTEST_SUITE(alp_ble_gatt_server, NULL, NULL, NULL, NULL, NULL);

static uint8_t find_attr_cb(const struct bt_gatt_attr *attr, uint16_t handle, void *user_data)
{
	const struct bt_gatt_attr **out = user_data;
	*out                            = attr;
	(void)handle;
	return BT_GATT_ITER_STOP;
}

static const struct bt_gatt_attr *find_attr(alp_ble_attr_handle_t handle)
{
	const struct bt_gatt_attr *attr = NULL;
	bt_gatt_foreach_attr(handle, handle, find_attr_cb, &attr);
	return attr;
}

static const alp_ble_ops_t *zephyr_ble_ops(void)
{
	const alp_backend_t *be = alp_backend_select("ble", ALP_SOC_REF_STR);
	zassert_not_null(be, "no ble backend registered for this build");
	zassert_equal(strcmp(be->vendor, "zephyr"), 0, "expected the zephyr_drv wildcard to win");
	return (const alp_ble_ops_t *)be->ops;
}

ZTEST(alp_ble_gatt_server, test_register_service_read_write)
{
	const alp_ble_ops_t *ops = zephyr_ble_ops();
	zassert_not_null(ops->gatt_register_service);

	static const uint8_t init_val[] = { 'h', 'i' };
	alp_ble_char_def_t   chars[1]   = {
		{
		    .uuid          = { .b = { 0xf2,
		                              0xde,
		                              0xbc,
		                              0x9a,
		                              0x78,
		                              0x56,
		                              0x34,
		                              0x12,
		                              0x78,
		                              0x56,
		                              0x34,
		                              0x12,
		                              0x78,
		                              0x56,
		                              0x34,
		                              0x12 } },
		    .properties    = ALP_BLE_GATT_PROP_READ | ALP_BLE_GATT_PROP_WRITE,
		    .initial_value = init_val,
		    .initial_len   = sizeof(init_val),
		},
	};
	alp_ble_service_def_t def = {
		.service_uuid = { .b = { 0xf0,
		                         0xde,
		                         0xbc,
		                         0x9a,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12 } },
		.chars        = chars,
		.num_chars    = ARRAY_SIZE(chars),
	};
	alp_ble_attr_handle_t handles[1] = { 0 };

	alp_ble_radio_state_t state = { .ops = ops, .be_data = NULL };
	zassert_equal(ops->gatt_register_service(&state, &def, handles), ALP_OK);
	zassert_not_equal(handles[0], 0, "registration must assign a real attribute handle");

	const struct bt_gatt_attr *attr = find_attr(handles[0]);
	zassert_not_null(attr, "value attribute must be discoverable at its assigned handle");
	zassert_not_null(attr->read);
	zassert_not_null(attr->write);

	/* Read back the initial value (registration seeded it). */
	uint8_t buf[16] = { 0 };
	ssize_t n       = attr->read(NULL, attr, buf, sizeof(buf), 0);
	zassert_equal(n, sizeof(init_val));
	zassert_mem_equal(buf, init_val, sizeof(init_val));

	/* Write a new value through the attribute's write() callback --
     * this is the exact call path a remote GATT client's ATT Write
     * Request drives (conn == NULL emulates a local/test caller, same
     * convention upstream's own gatt.c test uses). */
	static const uint8_t new_val[] = { 'H', 'I', '!' };
	ssize_t              wn        = attr->write(NULL, attr, new_val, sizeof(new_val), 0, 0);
	zassert_equal(wn, sizeof(new_val));

	/* Prove the write landed: read back through the same callback. */
	memset(buf, 0, sizeof(buf));
	n = attr->read(NULL, attr, buf, sizeof(buf), 0);
	zassert_equal(n, sizeof(new_val));
	zassert_mem_equal(buf, new_val, sizeof(new_val));
}

ZTEST(alp_ble_gatt_server, test_register_service_with_notify_char_assigns_distinct_handles)
{
	/* A second characteristic with NOTIFY set inserts an extra CCC
     * attribute -- this proves the attrs_needed/idx bookkeeping in
     * z_gatt_register_service() doesn't corrupt the FOLLOWING
     * characteristic's handle assignment. */
	const alp_ble_ops_t *ops = zephyr_ble_ops();

	alp_ble_char_def_t chars[2] = {
		{
		    .uuid       = { .b = { 0xf6,
		                           0xde,
		                           0xbc,
		                           0x9a,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12 } },
		    .properties = ALP_BLE_GATT_PROP_NOTIFY,
		},
		{
		    .uuid       = { .b = { 0xf7,
		                           0xde,
		                           0xbc,
		                           0x9a,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12,
		                           0x78,
		                           0x56,
		                           0x34,
		                           0x12 } },
		    .properties = ALP_BLE_GATT_PROP_READ,
		},
	};
	alp_ble_service_def_t def = {
		.service_uuid = { .b = { 0xf1,
		                         0xde,
		                         0xbc,
		                         0x9a,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12,
		                         0x78,
		                         0x56,
		                         0x34,
		                         0x12 } },
		.chars        = chars,
		.num_chars    = ARRAY_SIZE(chars),
	};
	alp_ble_attr_handle_t handles[2] = { 0 };

	alp_ble_radio_state_t state = { .ops = ops, .be_data = NULL };
	zassert_equal(ops->gatt_register_service(&state, &def, handles), ALP_OK);
	zassert_not_equal(handles[0], 0);
	zassert_not_equal(handles[1], 0);
	zassert_true(handles[1] > handles[0], "second characteristic's handle must follow the first");

	const struct bt_gatt_attr *attr1 = find_attr(handles[1]);
	zassert_not_null(attr1);
	zassert_not_null(attr1->read);
	zassert_is_null(attr1->write, "READ-only characteristic must not get a write callback");
}

/* Client GATT read (conn-side z_gatt_read()/ble_read_cb()) regression tests
 * -- issue #480 review. native_sim ships no BLE controller, so there is no
 * live peer connection to drive bt_gatt_read() end-to-end; these drive
 * ble_read_cb() directly via the CONFIG_ZTEST-only seam, reproducing the
 * exact (err, data, length) shapes Zephyr's gatt_read_rsp()
 * (subsys/bluetooth/host/gatt.c) delivers. */

ZTEST(alp_ble_gatt_server, test_client_read_cb_success_does_not_time_out)
{
	/* BUG 1: returning BT_GATT_ITER_STOP from the data-bearing branch
	 * used to suppress gatt_read_rsp()'s terminal func(..., NULL, 0)
	 * completion, so a successful read never signalled its semaphore
	 * and z_gatt_read() blocked to ALP_ERR_TIMEOUT on every success. */
	static const uint8_t data[] = { 0xAA, 0xBB };
	zassert_equal(alp_ble_test_read_cb(0, data, sizeof(data)),
	              ALP_OK,
	              "a successful client GATT read must not surface as ALP_ERR_TIMEOUT");
}

ZTEST(alp_ble_gatt_server, test_client_read_cb_att_error_maps_to_io)
{
	/* BUG 2: checking `data == NULL` before `err != 0` made the ATT
	 * error branch dead code (Zephyr delivers a rejected read as
	 * func(conn, err, params, NULL, 0) -- data is NULL there too), so a
	 * peer-rejected read surfaced as ALP_ERR_TIMEOUT instead of
	 * ALP_ERR_IO. */
	zassert_equal(alp_ble_test_read_cb(BT_ATT_ERR_INVALID_HANDLE, NULL, 0),
	              ALP_ERR_IO,
	              "a peer-rejected read must surface as ALP_ERR_IO, not ALP_ERR_TIMEOUT");
}

/* GATT read/write context pool regression tests -- issue #1939.
 * changelog.d/1620.md records the abandon path, the alp_lifecycle_cas()
 * loss and the ALP_ERR_BUSY refusal as unexecuted anywhere: native_sim
 * has no BLE controller, so no real peer can drive a late callback race
 * or a timeout. Each test below drives the exact backend function the
 * real path would reach, with a synthetic ctx standing in for the part
 * that needs a live connection (see the CONFIG_ZTEST seams this calls,
 * in src/backends/ble/zephyr_drv.c, for why each one is safe offline). */

ZTEST(alp_ble_gatt_server, test_client_read_timeout_abandons_procedure)
{
	/* Branch 1, "the abandon path": z_gatt_read()'s k_sem_take() deadline
	 * passes before ble_read_cb() ever fires, so the caller must win the
	 * LIVE->ABANDONED CAS and return ALP_ERR_TIMEOUT with the slot left
	 * claimed (asserted inside the seam) rather than freed. */
	zassert_equal(alp_ble_test_read_timeout(),
	              ALP_ERR_TIMEOUT,
	              "an abandoned read must surface as ALP_ERR_TIMEOUT");
}

ZTEST(alp_ble_gatt_server, test_client_write_timeout_abandons_procedure)
{
	/* Write-side twin of the read timeout test above. */
	zassert_equal(alp_ble_test_write_timeout(),
	              ALP_ERR_TIMEOUT,
	              "an abandoned write must surface as ALP_ERR_TIMEOUT");
}

ZTEST(alp_ble_gatt_server, test_client_read_cb_after_abandon_loses_cas)
{
	/* Branch 2, "the CAS loss": ble_read_cb() fires after the caller has
	 * already abandoned the procedure (ctx->state == BLE_PROC_ABANDONED).
	 * Its own LIVE->DONE CAS must lose, so a real data-bearing callback
	 * here must NOT be delivered -- it must return via _read_ctx_free()
	 * untouched, which the seam verifies by checking the caller-visible
	 * buffer was never written and ctx->done was never signalled. */
	static const uint8_t late_data[] = { 0xCC, 0xDD };
	zassert_equal(alp_ble_test_read_cb_after_abandon(0, late_data, sizeof(late_data)),
	              ALP_ERR_TIMEOUT,
	              "a callback losing the CAS to an already-abandoned ctx must not "
	              "deliver its result");
}

ZTEST(alp_ble_gatt_server, test_client_write_cb_after_abandon_loses_cas)
{
	/* Write-side twin of the read CAS-loss test above. */
	zassert_equal(alp_ble_test_write_cb_after_abandon(0),
	              ALP_ERR_TIMEOUT,
	              "a callback losing the CAS to an already-abandoned ctx must not "
	              "deliver its result");
}

ZTEST(alp_ble_gatt_server, test_client_gatt_read_busy_when_pool_exhausted)
{
	/* Branch 3, "the ALP_ERR_BUSY refusal": z_gatt_read() must refuse
	 * with ALP_ERR_BUSY when _read_ctx_alloc() finds every slot claimed
	 * -- reached through the real registered op, not a stand-in, since
	 * the busy check runs before anything that would need a live
	 * bt_conn. */
	const alp_ble_ops_t *ops     = zephyr_ble_ops();
	alp_ble_conn_state_t conn_st = { .radio   = NULL,
		                             .be_data = alp_ble_test_fake_conn_be(),
		                             .ops     = ops };
	uint8_t              buf[4];
	size_t               out_len = 0;

	alp_ble_test_set_ctx_pools_exhausted(true);
	alp_status_t rc = ops->gatt_read(&conn_st, 1, buf, sizeof(buf), &out_len, 10);
	alp_ble_test_set_ctx_pools_exhausted(false);

	zassert_equal(rc, ALP_ERR_BUSY, "gatt_read against an exhausted ctx pool must be ALP_ERR_BUSY");
}

ZTEST(alp_ble_gatt_server, test_client_gatt_write_busy_when_pool_exhausted)
{
	/* Write-side twin of the read ALP_ERR_BUSY test above. */
	const alp_ble_ops_t *ops     = zephyr_ble_ops();
	alp_ble_conn_state_t conn_st = { .radio   = NULL,
		                             .be_data = alp_ble_test_fake_conn_be(),
		                             .ops     = ops };
	static const uint8_t data[]  = { 0x01 };

	alp_ble_test_set_ctx_pools_exhausted(true);
	alp_status_t rc = ops->gatt_write(&conn_st, 1, data, sizeof(data), 10);
	alp_ble_test_set_ctx_pools_exhausted(false);

	zassert_equal(
	    rc, ALP_ERR_BUSY, "gatt_write against an exhausted ctx pool must be ALP_ERR_BUSY");
}
