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

alp_status_t alp_mqtt_loop(alp_mqtt_t *h, uint32_t timeout_ms)
{
	/* Counted via alp_handle_op_enter/leave -- same rationale as
	 * alp_mqtt_connect() above: loop() blocks up to timeout_ms polling
	 * the broker. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->loop == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->loop(&h->state, timeout_ms);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_mqtt_close(alp_mqtt_t *h)
{
	if (h == NULL) return;
	/* Sleep-poll drain (issue #629): this pool now counts
	 * alp_mqtt_connect()/alp_mqtt_loop(), each of which can block for a
	 * genuinely long time (a real broker round-trip / keepalive wait),
	 * so alp_handle_begin_close_blocking() sleeps between polls instead
	 * of busy-spinning (same rationale as rpc_dispatch.c's _rpc_drain(),
	 * GHSA-xhm8). Idempotent: a second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) return;
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
