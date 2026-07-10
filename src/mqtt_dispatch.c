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
		if (!_mqtt_pool[i].in_use) {
			memset(&_mqtt_pool[i], 0, sizeof(_mqtt_pool[i]));
			_mqtt_pool[i].in_use = true;
			return &_mqtt_pool[i];
		}
	}
	return NULL;
}

static void _free_mqtt(struct alp_mqtt *h)
{
	h->in_use = false;
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
	return h;
}

alp_status_t alp_mqtt_connect(alp_mqtt_t *h, uint32_t timeout_ms)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
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
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (topic == NULL) return ALP_ERR_INVAL;
	if (payload == NULL && len > 0) return ALP_ERR_INVAL;
	if (h->state.ops == NULL || h->state.ops->publish == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->publish(&h->state, topic, payload, len, qos, retain);
}

alp_status_t alp_mqtt_subscribe(alp_mqtt_t       *h,
                                const char       *topic_filter,
                                alp_mqtt_qos_t    qos,
                                alp_mqtt_msg_cb_t cb,
                                void             *user)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (topic_filter == NULL || cb == NULL) return ALP_ERR_INVAL;
	if (h->state.ops == NULL || h->state.ops->subscribe == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->subscribe(&h->state, topic_filter, qos, cb, user);
}

alp_status_t alp_mqtt_loop(alp_mqtt_t *h, uint32_t timeout_ms)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->loop == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->loop(&h->state, timeout_ms);
}

void alp_mqtt_close(alp_mqtt_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free_mqtt(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_mqtt_capabilities(const alp_mqtt_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
