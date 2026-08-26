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
 * with alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c):
 * a sleep-poll drain (never busy-spins -- issue #1114: unsafe
 * unconditionally, not just for a long-running op), generalised from
 * rpc_dispatch.c's _rpc_op_enter()/_rpc_begin_close()/_rpc_drain()
 * (GHSA-xhm8), which is safe to wait on a genuinely long-running op
 * instead of spinning the closer thread for the whole handshake. A
 * racing close/disconnect can no longer tear down state while one of
 * these ops is in flight.
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
 *
 * @par Issue #1118 -- repeated alp_ble_open() must join the singleton
 * <alp/ble.h> documents alp_ble_open() as returning the SAME pointer on
 * every call (BLE is a system singleton) and alp_ble_close() as only
 * tearing the controller down on the last matching close.  _alloc_radio()
 * is a plain pool allocator with no notion of "already open" -- with the
 * default pool size of 1, a second open() used to find the slot claimed
 * and surface ALP_ERR_NOMEM instead of the same pointer.  alp_ble_open()
 * now first tries to JOIN an already-open radio via an atomic
 * increment-if-nonzero on struct alp_ble::refcount, falling back to
 * _alloc_radio() (a genuine first open) only when no joinable radio
 * exists; alp_ble_close() decrement-and-checks the same counter and only
 * runs ops->close()/frees the slot when it was the last reference.  The
 * increment-if-nonzero / decrement-and-check-last pair is deliberately
 * NOT gated through the lifecycle byte the way op_enter/op_leave are --
 * doing that would let a joiner "resurrect" a handle a closer has
 * already committed to tearing down (the closer decrements to 0, THEN
 * transitions lifecycle to CLOSING; a lifecycle-gated joiner could slip
 * in between and receive a pointer to a handle mid-teardown). A CAS-loop
 * that only ever increments a refcount still visibly nonzero cannot
 * succeed once a decrement to zero has already committed -- see
 * alp_ble_open()/alp_ble_close() below.
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

/* issue #1118: increment-if-nonzero join of an already-open singleton --
 * see this file's "Issue #1118" header comment for why this is a
 * standalone CAS-loop on refcount alone rather than routed through
 * alp_handle_op_enter()'s lifecycle-gated counting. Returns true (h now
 * holds one more reference) or false (refcount was already 0: h is
 * unopened, or a close() has already committed to tearing it down --
 * caller must not touch h and should try the next slot / open fresh). */
static bool _radio_try_join(struct alp_ble *h)
{
	uint32_t cur = __atomic_load_n(&h->refcount, __ATOMIC_ACQUIRE);
	do {
		if (cur == 0u) return false;
	} while (!__atomic_compare_exchange_n(
	    &h->refcount, &cur, cur + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	return true;
}

/* issue #1118 round-2 dev review: decrement-if-nonzero, the release
 * half of _radio_try_join() above. alp_ble_close() used to
 * unconditionally __atomic_fetch_sub() refcount, which underflows to
 * ~0u on a double/stray close (against <alp/ble.h>'s "second close on
 * an already-closed handle is a safe no-op" contract) -- and a
 * corrupted, large-nonzero refcount is exactly what could let a
 * concurrent alp_ble_open()'s _radio_try_join() "join" a handle that
 * is mid-teardown (in_use still true, real refcount already 0), the
 * resurrection this file's #1118 header comment says the
 * increment-if-nonzero design must prevent. Mirroring _radio_try_join()
 * as a CAS loop means a close on an already-0 refcount touches nothing
 * and simply reports "not mine to tear down".
 * @return true if THIS call was the one that decremented refcount from
 *         1 to 0 (caller now owns the teardown); false if refcount was
 *         already 0 (no-op) or is still >0 after decrementing (another
 *         reference is still live). */
static bool _radio_try_leave(struct alp_ble *h)
{
	uint32_t cur = __atomic_load_n(&h->refcount, __ATOMIC_ACQUIRE);
	do {
		if (cur == 0u) return false;
	} while (!__atomic_compare_exchange_n(
	    &h->refcount, &cur, cur - 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	return cur == 1u;
}

alp_ble_t *alp_ble_open(void)
{
	alp_z_clear_last_error();

	/* Issue #1118: BLE is a documented singleton -- try to join an
	 * already-open radio before claiming a fresh pool slot.
	 *
	 * Round-2 dev review: the first cut of this fix stopped here after
	 * one scan, so a SECOND alp_ble_open() racing the FIRST one's
	 * in-flight open() saw the exact TOCTOU window this function's
	 * own opener creates -- _alloc_radio() below claims the slot
	 * (in_use = true) before ops->open() runs, and refcount is not
	 * published until ops->open() succeeds. A racing scan in that
	 * window reads in_use == true (so it can't _alloc_radio() a "free"
	 * slot) and refcount == 0 (so _radio_try_join() declines) -- with
	 * the default size-1 pool that is every slot, so the racer fell
	 * through to _alloc_radio() and got NULL / ALP_ERR_NOMEM instead of
	 * the singleton pointer <alp/ble.h> documents. The same in_use==true
	 * / refcount==0 shape also occurs, briefly, while alp_ble_close()
	 * below is mid-teardown (refcount already decremented to 0, in_use
	 * not yet released) -- a slot in THAT window must not be joined
	 * (the closer already committed to tearing it down) but a fresh
	 * alp_ble_open() must still eventually succeed once the close
	 * finishes and releases the slot.
	 *
	 * Retry the scan with a real sleep between attempts (not a busy
	 * spin -- same #1114 rationale: a spinning racer at equal-or-higher
	 * priority than the opener/closer thread could starve it on
	 * Zephyr's preemptive scheduler) until no slot is left in that
	 * ambiguous "claimed but refcount says nothing yet" state, then
	 * fall through to the normal join-scan-once-more/allocate path
	 * below. With the default 1-entry pool this always resolves the
	 * single slot one way or the other; a larger pool may wait on a
	 * pending slot even when a different slot is already free to
	 * allocate -- correct, just not maximally concurrent (not worth the
	 * added complexity for a pool this SDK only ever configures to 1
	 * today). */
	for (;;) {
		bool pending = false;
		for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_HANDLES; ++i) {
			struct alp_ble *h = &_radio_pool[i];
			if (!__atomic_load_n(&h->in_use, __ATOMIC_ACQUIRE)) {
				continue;
			}
			if (_radio_try_join(h)) {
				return h;
			}
			/* in_use but refcount==0: either another thread's open() is
			 * still running (will publish refcount, or release in_use
			 * on failure) or another thread's close() is mid-teardown
			 * (will release in_use). Either way, this slot's state is
			 * not final yet -- keep waiting on it rather than treating
			 * the pool as full. */
			pending = true;
		}
		if (!pending) {
			break;
		}
		alp_slot_sleep_tick();
	}

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
	/* First (and, until now, only) reference. Publish refcount before
	 * lifecycle so a racing joiner's increment-if-nonzero CAS can only
	 * ever see a fully-populated handle once it observes refcount != 0. */
	__atomic_store_n(&h->refcount, 1u, __ATOMIC_RELEASE);
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

void alp_ble_close(alp_ble_t *h)
{
	if (h == NULL) return;
	/* Issue #1118: decrement-and-check-last via _radio_try_leave()
	 * (round-2 dev review: NOT a plain unconditional fetch_sub -- see
	 * that function's doc comment for why an unconditional decrement
	 * underflows on a double/stray close). Any reference but the last
	 * just drops its count and returns; only the caller that observes
	 * the count reaching 0 proceeds to actually tear the controller
	 * down. A stray extra close() past the true last reference reads
	 * refcount already 0 and no-ops without touching it, preserving the
	 * existing "second/never-opened close no-ops" contract. */
	if (!_radio_try_leave(h)) {
		return;
	}
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

/* Legacy (non-extended) BT LE adv PDU budget: 31 bytes total, made up of AD
 * structures -- Flags (always emitted) = 3, Complete Local Name (if
 * cfg->name) = 2 + strlen(name), Service UUIDs (if any, all 128-bit UUIDs
 * packed into one AD structure) = 2 + sizeof(alp_ble_uuid_t)*num_services.
 * The portable alp_ble_adv_config_t has no extended-advertising field, so
 * this cap applies to every backend today; enforced once here (not
 * per-backend) so Zephyr and CC3501E can't drift out of sync (#480/#888). */
#define ALP_BLE_ADV_LEGACY_MAX_LEN 31u

static size_t alp_ble_adv_data_size(const alp_ble_adv_config_t *cfg)
{
	size_t size = 3u; /* Flags AD structure: always emitted. */
	if (cfg->name != NULL && cfg->name[0] != '\0') {
		size += 2u + strlen(cfg->name);
	}
	if (cfg->services != NULL && cfg->num_services > 0u) {
		size += 2u + sizeof(alp_ble_uuid_t) * cfg->num_services;
	}
	return size;
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
	} else if (alp_ble_adv_data_size(cfg) > ALP_BLE_ADV_LEGACY_MAX_LEN) {
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
	if (conn == NULL) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_INVAL;
	}
	if (!alp_handle_op_enter(&conn->lifecycle, &conn->active_ops)) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_NOT_READY;
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
	 * real link-layer handshake; alp_ble_close() drains this op with
	 * the sleep-poll alp_handle_begin_close_blocking() (never a
	 * busy-spin), so counting a long-running op here does not risk
	 * spinning the closer thread for the whole handshake. */
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
