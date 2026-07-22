/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT class dispatcher.  Owns the public <alp/iot.h> MQTT client
 * surface (alp_mqtt_t) on top of the backend registry mechanism
 * shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: MQTT lives in its own class registry
 * separate from Wi-Fi (alp_wifi_*) because it is a per-broker
 * client, not a hardware peripheral -- multiple alp_mqtt_t handles
 * can coexist against different brokers on the same radio.
 *
 * Slice 4b ships no vendor extensions for MQTT: the Zephyr backend
 * wraps the portable mqtt_client subsystem which works across every
 * E1M SoM that ships a TCP stack; no SoC-specific second tier is
 * needed.
 *
 * @par Issue #629 -- blocking ops counted + sleep-poll drained
 * alp_mqtt_publish()/alp_mqtt_subscribe()/alp_mqtt_connect()/
 * alp_mqtt_loop() are ALL bracketed with the standard
 * alp_handle_op_enter()/alp_handle_op_leave() guard from
 * src/common/alp_slot_claim.h, INCLUDING alp_mqtt_connect()/
 * alp_mqtt_loop(), each of which takes a timeout_ms that can block for
 * a genuinely long time (a real broker round-trip / keepalive wait).
 * alp_mqtt_close() drains the pool with
 * alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c)
 * instead of the busy-spin alp_handle_begin_close(): a sleep-poll
 * drain, generalised from rpc_dispatch.c's _rpc_op_enter()/
 * _rpc_begin_close()/_rpc_drain() (GHSA-xhm8), safe to wait on a
 * multi-second (or timeout_ms == forever) op instead of spinning the
 * closer thread.  A racing close() can no longer tear down state while
 * connect()/loop() is in flight.
 *
 * @par Issue #756 -- callback self-close inside alp_mqtt_loop()
 * Both the Yocto Mosquitto and Zephyr MQTT backends invoke the
 * subscribed message callback SYNCHRONOUSLY, inline, from inside their
 * ops->loop() implementation, before returning control here.  A
 * callback that calls alp_mqtt_close() on its OWN handle used to
 * deadlock: alp_mqtt_close()'s sleep-poll drain waits for active_ops
 * to reach 0, but the count it is waiting on is THIS very loop() call,
 * on THIS very thread, which cannot decrement until the callback
 * returns -- and it cannot return while it is asleep inside close().
 * alp_mqtt_loop() now brackets the backend call with
 * alp_handle_cb_enter()/alp_handle_cb_leave() (src/common/
 * alp_slot_claim.h) so alp_mqtt_close() can tell a reentrant self-close
 * (defer the actual teardown to this function's own post-op check)
 * from a genuine cross-thread close (unchanged: block and drain, same
 * as before). See alp_handle_begin_close_selfaware()'s doc comment for
 * the full contract; public re-entry guarantees are documented on
 * alp_mqtt_msg_cb_t in include/alp/iot.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/mqtt/mqtt_ops.h"

ALP_BACKEND_DEFINE_CLASS(mqtt);
/* Pull the mqtt registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(mqtt);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_MQTT_HANDLES
#define CONFIG_ALP_SDK_MAX_MQTT_HANDLES 2
#endif

static struct alp_mqtt _mqtt_pool[CONFIG_ALP_SDK_MAX_MQTT_HANDLES];

static struct alp_mqtt *_alloc_mqtt(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_MQTT_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_mqtt_pool[i].in_use)) {
			memset(&_mqtt_pool[i], 0, offsetof(struct alp_mqtt, in_use));
			return &_mqtt_pool[i];
		}
	}
	return NULL;
}

static void _free_mqtt(struct alp_mqtt *h)
{
	alp_slot_release(&h->in_use);
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->broker_uri == NULL || cfg->client_id == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("mqtt", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_mqtt_ops_t *ops = (const alp_mqtt_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_mqtt *h = _alloc_mqtt();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.cfg            = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_mqtt(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_mqtt_connect(alp_mqtt_t *h, uint32_t timeout_ms)
{
	/* Counted via alp_handle_op_enter/leave -- see this file's "Issue
	 * #629" header comment: connect() can block up to timeout_ms on a
	 * real broker handshake; alp_mqtt_close() now drains this op with
	 * the sleep-poll alp_handle_begin_close_blocking() instead of the
	 * busy-spin alp_handle_begin_close(). */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->connect == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->connect(&h->state, timeout_ms);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_mqtt_publish(alp_mqtt_t    *h,
                              const char    *topic,
                              const uint8_t *payload,
                              size_t         len,
                              alp_mqtt_qos_t qos,
                              bool           retain)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_mqtt/_free_mqtt, so mixing
	 * it with a plain read here is a data race, and a racing close
	 * could free the slot mid-op. op_enter counts this op in;
	 * begin_close drains it. #629 */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (topic == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (payload == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->publish == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->publish(&h->state, topic, payload, len, qos, retain);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_mqtt_subscribe(alp_mqtt_t       *h,
                                const char       *topic_filter,
                                alp_mqtt_qos_t    qos,
                                alp_mqtt_msg_cb_t cb,
                                void             *user)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (topic_filter == NULL || cb == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->subscribe == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->subscribe(&h->state, topic_filter, qos, cb, user);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

/* Test-only hook (dev-review follow-up on issue #756): fires right
 * after alp_handle_op_leave() below -- the exact point from which this
 * thread must never touch `h` again unless it uniquely owns the close
 * (self_closed) -- so tests/yocto/mqtt_dispatch_self_close.c's
 * cross-thread-close-vs-slot-reuse scenario can widen that window
 * deterministically (a racing external closer's sleep-poll drain reacts
 * on a millisecond cadence; this makes the reproduction of a regression
 * to the pre-fix ordering independent of that cadence, instead of
 * relying on raw thread-scheduling luck -- mirrors
 * src/backends/rpc/yocto_drv.c's g_y_call_test_late_staging_hook idiom).
 * NULL in production. */
static void (*g_mqtt_test_after_op_leave_hook)(void) = NULL;

alp_status_t alp_mqtt_loop(alp_mqtt_t *h, uint32_t timeout_ms)
{
	/* Counted via alp_handle_op_enter/leave -- same rationale as
	 * alp_mqtt_connect() above: loop() blocks up to timeout_ms polling
	 * the broker. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	/* Issue #756: loop() may synchronously invoke the subscribed message
	 * callback, which may call alp_mqtt_close() on THIS handle -- mark
	 * this thread as "inside the callback-invoking op" first so that
	 * close() can detect the reentrant self-close and defer instead of
	 * deadlocking (see this file's header comment). */
	alp_handle_cb_enter(&h->cb_thread, &h->cb_active);
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->loop == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->loop(&h->state, timeout_ms);
	alp_handle_cb_leave(&h->cb_active);
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
	if (g_mqtt_test_after_op_leave_hook != NULL) {
		g_mqtt_test_after_op_leave_hook();
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
		_free_mqtt(h);
	}
	return rc;
}

void alp_mqtt_close(alp_mqtt_t *h)
{
	if (h == NULL) return;
	/* Self-close-aware sleep-poll drain (issues #629/#756): this pool
	 * counts alp_mqtt_connect()/alp_mqtt_loop(), each of which can block
	 * for a genuinely long time (a real broker round-trip / keepalive
	 * wait), so a normal (external) close sleeps between polls instead
	 * of busy-spinning (same rationale as rpc_dispatch.c's _rpc_drain(),
	 * GHSA-xhm8).  A close triggered BY a message callback running
	 * inside this handle's own alp_mqtt_loop() is detected instead and
	 * deferred to that loop() call's own post-callback check (see
	 * alp_mqtt_loop() above) -- draining here would deadlock on this
	 * same thread's own in-flight count.  Idempotent either way: a
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
	_free_mqtt(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_mqtt_capabilities(const alp_mqtt_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
