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
 * @par Issue #629 -- partial conversion, flagged for orchestrator review
 * alp_mqtt_publish()/alp_mqtt_subscribe() are bracketed with the
 * standard alp_handle_op_enter()/alp_handle_op_leave() guard from
 * src/common/alp_slot_claim.h, and alp_mqtt_close() uses that header's
 * alp_handle_begin_close() to drain them.  alp_mqtt_connect() and
 * alp_mqtt_loop() are deliberately NOT counted in that same guard:
 * both take a timeout_ms that can block for a genuinely long time (a
 * real broker round-trip / keepalive wait), and alp_handle_begin_close()
 * is a documented busy-spin whose precondition is "every counted op is
 * a short, synchronous backend call" -- counting a multi-second (or
 * timeout_ms == forever) op there would turn alp_mqtt_close() into an
 * unbounded spin instead of a bounded drain.  They fall back to a
 * lifecycle-byte-only check (alp_lifecycle_get()) -- strictly better
 * than the old unlocked `in_use` read, but it does NOT stop a racing
 * close() from tearing down state while connect()/loop() is in
 * flight.  Closing this gap for real needs rpc_dispatch.c's dedicated
 * counted lifecycle word + sleep-poll drain (see that file's
 * _rpc_op_enter/_rpc_begin_close/_rpc_drain, added for GHSA-xhm8),
 * not this shared spin-based helper -- left as a follow-up rather than
 * forcing a mismatched fix here.
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
	/* NOT bracketed with alp_handle_op_enter/leave -- see this file's
	 * "Issue #629" header comment: connect() can block up to
	 * timeout_ms on a real broker handshake, and alp_handle_begin_close()
	 * is documented as a busy-spin valid only for short, synchronous
	 * ops.  Lifecycle-byte-only check: an improvement over the old
	 * unlocked in_use read, but a racing close() is not blocked from
	 * tearing down state underneath an in-flight connect(). Flagged
	 * for orchestrator review. */
	if (h == NULL || alp_lifecycle_get(&h->lifecycle) != ALP_HANDLE_LC_OPEN) {
		return ALP_ERR_NOT_READY;
	}
	if (h->state.ops == NULL || h->state.ops->connect == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->connect(&h->state, timeout_ms);
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
	/* NOT bracketed -- same rationale as alp_mqtt_connect() above: loop()
	 * blocks up to timeout_ms polling the broker. Flagged for
	 * orchestrator review. */
	if (h == NULL || alp_lifecycle_get(&h->lifecycle) != ALP_HANDLE_LC_OPEN) {
		return ALP_ERR_NOT_READY;
	}
	if (h->state.ops == NULL || h->state.ops->loop == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->loop(&h->state, timeout_ms);
}

void alp_mqtt_close(alp_mqtt_t *h)
{
	if (h == NULL) return;
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left (publish/subscribe only -- see
	 * this file's header comment for why connect/loop are not counted)
	 * -- so teardown never races an in-flight publish/subscribe.
	 * Idempotent: a second/never-opened close no-ops. #629 */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) return;
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
