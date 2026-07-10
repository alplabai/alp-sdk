/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi class dispatcher.  Owns the public <alp/iot.h> Wi-Fi
 * station surface (alp_wifi_t) on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry, one handle type
 * -- Wi-Fi station is single-instance per E1M-conformant SoM (one
 * radio per module).  The MQTT half of <alp/iot.h> ships as its
 * own class registry (alp_mqtt_* in src/mqtt_dispatch.c) since
 * MQTT is a per-broker client, not a hardware peripheral.
 *
 * AEN's CC3501E coprocessor is a real registry backend
 * (src/backends/wifi/cc3501e.c) that wraps chips/cc3501e after the
 * application attaches the live bridge handle.  Other Zephyr targets
 * continue to use the wildcard wifi_mgmt backend.
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

#include "backends/wifi/wifi_ops.h"

ALP_BACKEND_DEFINE_CLASS(wifi);
/* Pull the wifi registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(wifi);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_WIFI_HANDLES
#define CONFIG_ALP_SDK_MAX_WIFI_HANDLES 1
#endif

static struct alp_wifi _radio_pool[CONFIG_ALP_SDK_MAX_WIFI_HANDLES];

static struct alp_wifi *_alloc_radio(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_WIFI_HANDLES; ++i) {
		if (!_radio_pool[i].in_use) {
			memset(&_radio_pool[i], 0, sizeof(_radio_pool[i]));
			_radio_pool[i].in_use = true;
			return &_radio_pool[i];
		}
	}
	return NULL;
}

static void _free_radio(struct alp_wifi *h)
{
	h->in_use = false;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_wifi_t *alp_wifi_open(void)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("wifi", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_wifi_ops_t *ops = (const alp_wifi_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_wifi *h = _alloc_radio();
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
	return h;
}

alp_status_t
alp_wifi_connect(alp_wifi_t *h, const alp_wifi_credentials_t *creds, uint32_t timeout_ms)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (creds == NULL || creds->ssid == NULL) return ALP_ERR_INVAL;
	if (h->state.ops == NULL || h->state.ops->connect == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->connect(&h->state, creds, timeout_ms);
}

alp_status_t alp_wifi_disconnect(alp_wifi_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->disconnect == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->disconnect(&h->state);
}

void alp_wifi_close(alp_wifi_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free_radio(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_wifi_capabilities(const alp_wifi_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
