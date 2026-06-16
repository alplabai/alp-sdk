/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr Net Wi-Fi backend for the <alp/iot.h> Wi-Fi
 * surface.  Lifts the body of src/zephyr/iot_zephyr.c (the legacy
 * v0.2 wrapper) into a registry-shaped backend.  Registers as
 * silicon_ref="*" at priority 100 -- mirrors the design spec
 * Section 2 backend matrix (zephyr_drv wins on every SoC unless a
 * more specific backend registers).
 *
 * V2N CC3501E note: the CC3501E Wi-Fi 6 + BLE 5.4 coprocessor on
 * the AEN SoM is wired into Zephyr's DT as a tilab,cc3501-wifi-class
 * node so net_mgmt dispatches the wifi_mgmt requests through TI's
 * controller-side driver transparently; the Zephyr backend handles
 * V2N without a separate registry entry -- no <alp/ext/...> header
 * and no vendor extension.
 *
 * Gated on CONFIG_ALP_SDK_IOT_WIFI -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with WiFi
 * in the device tree.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "wifi_ops.h"

#if defined(CONFIG_ALP_SDK_IOT_WIFI)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

struct wifi_radio_be {
#if defined(CONFIG_ALP_SDK_IOT_WIFI)
	struct net_if                 *iface;
	struct net_mgmt_event_callback wifi_cb;
	struct k_sem                   connect_sem;
	int                            connect_status; /* 0 = up, errno-style on failure */
#else
	int unused;
#endif
};

/* Singleton backend state -- Wi-Fi station is single-instance per
 * E1M-conformant SoM (one radio per module), so a single static
 * struct is the right shape.  The dispatcher's handle pool sizes
 * the customer-visible alp_wifi_t pool independently. */
#if defined(CONFIG_ALP_SDK_IOT_WIFI)

static struct wifi_radio_be _radio_be;

static alp_status_t errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case -ENOMEM:
		return ALP_ERR_NOMEM;
	default:
		return ALP_ERR_IO;
	}
}

static void
wifi_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event, struct net_if *iface)
{
	(void)iface;
	struct wifi_radio_be *be = CONTAINER_OF(cb, struct wifi_radio_be, wifi_cb);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;
		be->connect_status               = (status != NULL) ? status->status : -EIO;
		k_sem_give(&be->connect_sem);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		/* Disconnect signal -- keep status field untouched so a pending
         * connect()'s wait can surface its own result. */
		break;
	default:
		break;
	}
}
#endif /* CONFIG_ALP_SDK_IOT_WIFI */

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t z_open(alp_wifi_backend_state_t *st, alp_capabilities_t *caps_out)
{
#if defined(CONFIG_ALP_SDK_IOT_WIFI)
	struct wifi_radio_be *be = &_radio_be;
	memset(be, 0, sizeof(*be));

	be->iface = net_if_get_default();
	if (be->iface == NULL) {
		caps_out->flags = 0u;
		return ALP_ERR_NOT_READY;
	}

	k_sem_init(&be->connect_sem, 0, 1);
	net_mgmt_init_event_callback(&be->wifi_cb,
	                             wifi_event_handler,
	                             NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&be->wifi_cb);
	st->be_data     = be;
	caps_out->flags = 0u;
	return ALP_OK;
#else
	(void)st;
	caps_out->flags = 0u;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_connect(alp_wifi_backend_state_t *st, const alp_wifi_credentials_t *creds, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_IOT_WIFI)
	struct wifi_radio_be *be = (struct wifi_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	struct wifi_connect_req_params params = { 0 };
	params.ssid                           = (const uint8_t *)creds->ssid;
	params.ssid_length                    = strlen(creds->ssid);
	params.channel                        = WIFI_CHANNEL_ANY;
	params.mfp                            = WIFI_MFP_OPTIONAL;
	if (creds->psk != NULL) {
		params.psk        = (const uint8_t *)creds->psk;
		params.psk_length = strlen(creds->psk);
		params.security   = WIFI_SECURITY_TYPE_PSK;
	} else {
		params.security = WIFI_SECURITY_TYPE_NONE;
	}

	k_sem_reset(&be->connect_sem);
	be->connect_status = -EIO;

	int err = net_mgmt(NET_REQUEST_WIFI_CONNECT, be->iface, &params, sizeof(params));
	if (err != 0) {
		return errno_to_alp(err);
	}

	if (k_sem_take(&be->connect_sem, K_MSEC(timeout_ms)) != 0) {
		return ALP_ERR_TIMEOUT;
	}

	return (be->connect_status == 0) ? ALP_OK : ALP_ERR_IO;
#else
	(void)st;
	(void)creds;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_disconnect(alp_wifi_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_IOT_WIFI)
	struct wifi_radio_be *be = (struct wifi_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	int err = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, be->iface, NULL, 0);
	return errno_to_alp(err);
#else
	(void)st;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_wifi_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_IOT_WIFI)
	struct wifi_radio_be *be = (struct wifi_radio_be *)st->be_data;
	if (be == NULL) return;
	net_mgmt_del_event_callback(&be->wifi_cb);
	st->be_data = NULL;
#else
	(void)st;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_wifi_ops_t _ops = {
	.open       = z_open,
	.connect    = z_connect,
	.disconnect = z_disconnect,
	.close      = z_close,
};

ALP_BACKEND_REGISTER(wifi,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
