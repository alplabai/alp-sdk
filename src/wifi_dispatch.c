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

#include "alp_slot_claim.h"
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
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_radio_pool[i].in_use)) {
			memset(&_radio_pool[i], 0, offsetof(struct alp_wifi, in_use));
			return &_radio_pool[i];
		}
	}
	return NULL;
}

static void _free_radio(struct alp_wifi *h)
{
	alp_slot_release(&h->in_use);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t
alp_wifi_connect(alp_wifi_t *h, const alp_wifi_credentials_t *creds, uint32_t timeout_ms)
{
	/* Counted via alp_handle_op_enter/leave (issue #629): association can
	 * block up to timeout_ms (a real SoM's SSID scan+auth handshake), so
	 * alp_wifi_close() drains this op with the sleep-poll
	 * alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c)
	 * instead of the busy-spin alp_handle_begin_close() -- generalised
	 * from rpc_dispatch.c's _rpc_op_enter()/_rpc_begin_close()/
	 * _rpc_drain() (GHSA-xhm8). A close() racing an in-flight connect()
	 * can no longer tear down state underneath it. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (creds == NULL || creds->ssid == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (h->state.ops == NULL || h->state.ops->connect == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->connect(&h->state, creds, timeout_ms);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_wifi_disconnect(alp_wifi_t *h)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_radio/_free_radio, so mixing
	 * it with a plain read here is a data race, and a racing close could
	 * free the slot mid-op. op_enter counts this op in; begin_close
	 * drains it. #629 */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops == NULL || h->state.ops->disconnect == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->disconnect(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_wifi_close(alp_wifi_t *h)
{
	if (h == NULL) return;
	/* Sleep-poll drain (issue #629): this pool counts alp_wifi_connect(),
	 * which can block up to its caller's timeout_ms, so
	 * alp_handle_begin_close_blocking() sleeps between polls instead of
	 * busy-spinning (same rationale as rpc_dispatch.c's _rpc_drain(),
	 * GHSA-xhm8). Idempotent: a second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_radio(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_wifi_capabilities(const alp_wifi_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
