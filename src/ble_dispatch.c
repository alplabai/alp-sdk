/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE class dispatcher.  Owns the public <alp/ble.h> surface --
 * both the radio singleton (alp_ble_t) and per-connection
 * (alp_ble_conn_t) handles -- on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers both
 * handle types since the controller is one piece of hardware.
 * The ops vtable carries function pointers for both surfaces; the
 * dispatcher maintains two separate handle pools (radio + conn).
 *
 * AEN's CC3501E coprocessor is a real registry backend
 * (src/backends/ble/cc3501e.c) that wraps chips/cc3501e after the
 * application attaches the live bridge handle.  Other Zephyr targets
 * continue to use the wildcard BT-host backend.
 *
 * @par Issue #629 -- blocking ops counted + sleep-poll drained
 * Both pools (radio: alp_ble_t, conn: alp_ble_conn_t) get the standard
 * alp_handle_op_enter()/alp_handle_op_leave() guard from
 * src/common/alp_slot_claim.h for every op, INCLUDING
 * alp_ble_connect() (radio-side) and alp_ble_gatt_read()/
 * alp_ble_gatt_write() (conn-side), each of which can block up to a
 * caller-supplied timeout_ms on a real link-layer round-trip.
 * alp_ble_close() / alp_ble_disconnect() therefore drain their pool
 * with alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c)
 * instead of the busy-spin alp_handle_begin_close(): a sleep-poll
 * drain, generalised from rpc_dispatch.c's _rpc_op_enter()/
 * _rpc_begin_close()/_rpc_drain() (GHSA-xhm8), which is safe to wait
 * on a genuinely long-running op instead of spinning the closer
 * thread for the whole handshake. A racing close/disconnect can no
 * longer tear down state while one of these ops is in flight.
 *
 * @par Issue #756 -- callback self-close inside alp_ble_scan_start()
 * The CC3501E backend's scan_start op runs the scan to completion and
 * then invokes the caller's scan callback SYNCHRONOUSLY, inline, once
 * per result, before returning control here.  A callback that calls
 * alp_ble_close() on its own radio handle used to deadlock the same
 * way alp_mqtt_loop()'s message callback did (issue #756): the
 * sleep-poll drain in alp_ble_close() waits for active_ops to reach 0,
 * but that count is THIS very scan_start() call, on THIS very thread,
 * which cannot decrement until the callback returns -- and it cannot
 * return while asleep inside close().  alp_ble_scan_start() now
 * brackets the backend call with alp_handle_cb_enter()/
 * alp_handle_cb_leave() (src/common/alp_slot_claim.h) so
 * alp_ble_close() can tell a reentrant self-close (defer teardown to
 * this function's own post-op check) from a genuine cross-thread close
 * (unchanged: block and drain, same as before).  Public re-entry
 * guarantees are documented on alp_ble_scan_cb_t in include/alp/ble.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/ble/ble_ops.h"

ALP_BACKEND_DEFINE_CLASS(ble);
/* Pull the ble registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(ble);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_BLE_HANDLES
#define CONFIG_ALP_SDK_MAX_BLE_HANDLES 1
#endif
#ifndef CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES
#define CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES 4
#endif

static struct alp_ble      _radio_pool[CONFIG_ALP_SDK_MAX_BLE_HANDLES];
static struct alp_ble_conn _conn_pool[CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES];

static struct alp_ble *_alloc_radio(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_radio_pool[i].in_use)) {
			memset(&_radio_pool[i], 0, offsetof(struct alp_ble, in_use));
			return &_radio_pool[i];
		}
	}
	return NULL;
}

static void _free_radio(struct alp_ble *h)
{
	alp_slot_release(&h->in_use);
}

static struct alp_ble_conn *_alloc_conn(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES; ++i) {
		/* Same atomic-claim contract as _alloc_radio() above. */
		if (alp_slot_try_claim(&_conn_pool[i].in_use)) {
			memset(&_conn_pool[i], 0, offsetof(struct alp_ble_conn, in_use));
			return &_conn_pool[i];
		}
	}
	return NULL;
}

static void _free_conn(struct alp_ble_conn *h)
{
	alp_slot_release(&h->in_use);
}

/* ================================================================== */
/* Radio-side dispatch                                                 */
/* ================================================================== */

alp_ble_t *alp_ble_open(void)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("ble", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_ble_ops_t *ops = (const alp_ble_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_ble *h = _alloc_radio();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(&h->state, &caps);
	if (rc != ALP_OK) {
		_free_radio(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

void alp_ble_close(alp_ble_t *h)
{
	if (h == NULL) return;
	/* Self-close-aware sleep-poll drain (issues #629/#756): this pool
	 * counts alp_ble_connect(), which can block up to its caller's
	 * timeout_ms, so a normal (external) close sleeps between polls
	 * instead of busy-spinning (same rationale as rpc_dispatch.c's
	 * _rpc_drain(), GHSA-xhm8).  A close triggered BY a scan callback
	 * running inside this handle's own alp_ble_scan_start() is detected
	 * instead and deferred to that call's own post-callback check (see
	 * alp_ble_scan_start() below) -- draining here would deadlock on
	 * this same thread's own in-flight count.  Idempotent either way: a
	 * second/never-opened close no-ops. */
	alp_handle_close_mode_t mode;
	if (!alp_handle_begin_close_selfaware(&h->lifecycle,
	                                      &h->active_ops,
	                                      &h->cb_active,
	                                      &h->cb_thread,
	                                      &mode,
	                                      &h->close_pending)) {
		return;
	}
	if (mode == ALP_HANDLE_CLOSE_DEFERRED) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_radio(h);
}

alp_status_t alp_ble_advertise_start(alp_ble_t *h, const alp_ble_adv_config_t *cfg)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_radio/_free_radio, so mixing
	 * it with a plain read here is a data race, and a racing close
	 * could free the slot mid-op. op_enter counts this op in;
	 * begin_close drains it. #629 */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (cfg == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->advertise_start == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->advertise_start(&h->state, cfg);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_ble_advertise_stop(alp_ble_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops == NULL || h->state.ops->advertise_stop == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->advertise_stop(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_ble_gatt_register_service(alp_ble_t                   *h,
                                           const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t       *handles_out)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (def == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->gatt_register_service == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->gatt_register_service(&h->state, def, handles_out);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_ble_gatt_notify(alp_ble_t            *h,
                                 alp_ble_conn_t       *conn,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t        *payload,
                                 size_t                len)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	/* gatt_notify is a SHORT synchronous op that dereferences conn->state, so
	 * it must count the conn handle too -- otherwise a racing
	 * alp_ble_disconnect(conn) drains conn->active_ops instantly (this op was
	 * never counted there) and recycles the conn slot beneath this call = UAF
	 * (issue #629). Count h then conn; op_enter never blocks, so the fixed
	 * h-before-conn order cannot deadlock. */
	if (conn == NULL || !alp_handle_op_enter(&conn->lifecycle, &conn->active_ops)) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_INVAL;
	}
	alp_status_t rc;
	if (payload == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->gatt_notify == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->gatt_notify(&h->state, &conn->state, handle, payload, len);
	}
	alp_handle_op_leave(&conn->active_ops);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

/* Test-only hook (dev-review follow-up on issue #756): fires right
 * after alp_handle_op_leave() below -- mirrors
 * src/mqtt_dispatch.c's g_mqtt_test_after_op_leave_hook (see its doc
 * comment for the full rationale). NULL in production. */
static void (*g_ble_test_after_op_leave_hook)(void) = NULL;

alp_status_t alp_ble_scan_start(alp_ble_t *h, bool active, alp_ble_scan_cb_t cb, void *user)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (cb == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->scan_start == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		/* Issue #756: the backend may synchronously invoke `cb` once per
		 * scan result before returning, and `cb` may call alp_ble_close()
		 * on THIS handle -- mark this thread as "inside the callback-
		 * invoking op" first so close() can detect the reentrant
		 * self-close and defer instead of deadlocking (see this file's
		 * header comment). */
		alp_handle_cb_enter(&h->cb_thread, &h->cb_active);
		rc = h->state.ops->scan_start(&h->state, active, cb, user);
		alp_handle_cb_leave(&h->cb_active);
	}
	/* Dev-review follow-up on issue #756: consume close_pending BEFORE
	 * op_leave, while this thread still holds its own counted op -- `h`
	 * cannot be freed by a concurrent EXTERNAL closer until active_ops
	 * drains to 0, which cannot happen before the op_leave below runs.
	 * Checking close_pending any later raced that external closer, which
	 * is free to tear `h` down the instant this op leaves -- a
	 * use-after-free/double-free window (see
	 * alp_handle_take_deferred_close()'s doc comment). */
	bool self_closed = alp_handle_take_deferred_close(&h->close_pending);
	alp_handle_op_leave(&h->active_ops);
	if (g_ble_test_after_op_leave_hook != NULL) {
		g_ble_test_after_op_leave_hook();
	}
	if (self_closed) {
		/* alp_handle_begin_close_selfaware()'s CAS already elected this
		 * thread the unique owner of this handle's close -- safe to
		 * drain (any OTHER concurrent op) and tear down now. */
		alp_handle_drain_blocking(&h->active_ops);
		if (h->state.ops != NULL && h->state.ops->close != NULL) {
			h->state.ops->close(&h->state);
		}
		alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
		_free_radio(h);
	}
	return rc;
}

alp_status_t alp_ble_scan_stop(alp_ble_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops == NULL || h->state.ops->scan_stop == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->scan_stop(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_ble_connect(alp_ble_t            *h,
                             const alp_ble_addr_t *peer,
                             uint32_t              timeout_ms,
                             alp_ble_conn_t      **conn_out)
{
	/* Counted via alp_handle_op_enter/leave -- see this file's "Issue
	 * #629" header comment. connect() can block up to timeout_ms on a
	 * real link-layer handshake; alp_ble_close() now drains this op
	 * with the sleep-poll alp_handle_begin_close_blocking() instead of
	 * the busy-spin alp_handle_begin_close(), so counting a
	 * long-running op here no longer risks spinning the closer thread
	 * for the whole handshake. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t         rc;
	struct alp_ble_conn *c = NULL;
	if (peer == NULL || conn_out == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->connect == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else if ((c = _alloc_conn()) == NULL) {
		rc = ALP_ERR_NOMEM;
	} else {
		c->backend     = h->backend;
		c->state.ops   = h->state.ops;
		c->state.radio = h;
		rc             = h->state.ops->connect(&h->state, peer, timeout_ms, &c->state);
		if (rc != ALP_OK) {
			_free_conn(c);
		} else {
			alp_lifecycle_set(&c->lifecycle, ALP_HANDLE_LC_OPEN);
			*conn_out = c;
		}
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

/* ================================================================== */
/* Conn-side dispatch                                                  */
/* ================================================================== */

alp_status_t alp_ble_disconnect(alp_ble_conn_t *c)
{
	if (c == NULL) return ALP_ERR_NOT_READY;
	/* alp_ble_disconnect() IS this pool's close/teardown op (it frees
	 * the conn slot), just with a non-void alp_status_t return the
	 * task's "no public signature change" constraint keeps as-is.
	 * Sleep-poll drain (issue #629): this pool now counts
	 * alp_ble_gatt_read()/alp_ble_gatt_write(), each of which can block
	 * up to its caller's timeout_ms, so alp_handle_begin_close_blocking()
	 * sleeps between polls instead of busy-spinning (same rationale as
	 * rpc_dispatch.c's _rpc_drain(), GHSA-xhm8) -- gatt_notify from the
	 * radio side also counts on conn->active_ops (see that op), so this
	 * drain waits on it too.  A lost CAS (already disconnecting/
	 * disconnected) matches the old !in_use -> NOT_READY contract. */
	if (!alp_handle_begin_close_blocking(&c->lifecycle, &c->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc = ALP_OK;
	if (c->state.ops != NULL && c->state.ops->disconnect != NULL) {
		rc = c->state.ops->disconnect(&c->state);
	}
	alp_lifecycle_set(&c->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_conn(c);
	return rc;
}

alp_status_t alp_ble_gatt_read(alp_ble_conn_t       *c,
                               alp_ble_attr_handle_t handle,
                               uint8_t              *out,
                               size_t                out_cap,
                               size_t               *out_len,
                               uint32_t              timeout_ms)
{
	if (out_len != NULL) *out_len = 0;
	/* Counted via alp_handle_op_enter/leave -- same rationale as
	 * alp_ble_connect() above: a GATT read can block up to timeout_ms
	 * on the peer's response, and alp_ble_disconnect() now drains this
	 * op with the sleep-poll alp_handle_begin_close_blocking(). */
	if (c == NULL || !alp_handle_op_enter(&c->lifecycle, &c->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (out == NULL && out_cap > 0) {
		rc = ALP_ERR_INVAL;
	} else if (c->state.ops == NULL || c->state.ops->gatt_read == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = c->state.ops->gatt_read(&c->state, handle, out, out_cap, out_len, timeout_ms);
	}
	alp_handle_op_leave(&c->active_ops);
	return rc;
}

alp_status_t alp_ble_gatt_write(alp_ble_conn_t       *c,
                                alp_ble_attr_handle_t handle,
                                const uint8_t        *data,
                                size_t                len,
                                uint32_t              timeout_ms)
{
	/* Counted via alp_handle_op_enter/leave -- same rationale as
	 * alp_ble_gatt_read() above. */
	if (c == NULL || !alp_handle_op_enter(&c->lifecycle, &c->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (data == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (c->state.ops == NULL || c->state.ops->gatt_write == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = c->state.ops->gatt_write(&c->state, handle, data, len, timeout_ms);
	}
	alp_handle_op_leave(&c->active_ops);
	return rc;
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_ble_capabilities(const alp_ble_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
