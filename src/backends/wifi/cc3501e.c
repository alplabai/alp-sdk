/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E Wi-Fi backend for the portable <alp/iot.h> station surface.
 *
 * The E1M-AEN family carries Wi-Fi in the on-module CC3501E companion, not in
 * the Alif Ensemble SoC.  This backend registers exact AEN silicon refs ahead
 * of the wildcard Zephyr wifi_mgmt backend and wraps chips/cc3501e.
 *
 * The live bridge handle is still application-owned.  AEN apps bring the
 * companion up once via cc3501e_bridge_bringup(); that helper then calls
 * alp_wifi_cc3501e_attach() so alp_wifi_open() can bind to the ready bridge.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "wifi_ops.h"

static cc3501e_t *g_bridge_ctx;

alp_status_t alp_wifi_cc3501e_attach(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_INVAL;
	g_bridge_ctx = ctx;
	return ALP_OK;
}

static alp_status_t cc35_open(alp_wifi_backend_state_t *state, alp_capabilities_t *caps_out)
{
	if (g_bridge_ctx == NULL) {
		caps_out->flags = 0u;
		return ALP_ERR_NOT_READY;
	}
	state->be_data  = g_bridge_ctx;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t cc35_connect(alp_wifi_backend_state_t     *state,
                                 const alp_wifi_credentials_t *creds,
                                 uint32_t                      timeout_ms)
{
	cc3501e_t *ctx = (cc3501e_t *)state->be_data;
	if (ctx == NULL) return ALP_ERR_NOT_READY;

	/* The portable v0.x credential shape has no security enum.  Keep open
	 * networks open, and use the CC3501E firmware's WPA2-PSK path when a PSK is
	 * supplied.  WPA3 remains available through the chip diagnostic API and the
	 * console command until the portable surface grows a security selector. */
	uint8_t     sec  = (creds->psk == NULL || creds->psk[0] == '\0') ? 0u : 1u;
	const char *pass = (creds->psk != NULL) ? creds->psk : "";
	return cc3501e_wifi_connect(ctx, creds->ssid, sec, pass, timeout_ms);
}

static alp_status_t cc35_disconnect(alp_wifi_backend_state_t *state)
{
	cc3501e_t *ctx = (cc3501e_t *)state->be_data;
	if (ctx == NULL) return ALP_ERR_NOT_READY;
	return cc3501e_wifi_disconnect(ctx);
}

static void cc35_close(alp_wifi_backend_state_t *state)
{
	state->be_data = NULL;
}

static const alp_wifi_ops_t _ops = {
	.open       = cc35_open,
	.connect    = cc35_connect,
	.disconnect = cc35_disconnect,
	.close      = cc35_close,
};

#define ALP_WIFI_CC3501E_REGISTER(_name, _ref) \
	ALP_BACKEND_REGISTER(wifi, \
	                     _name, \
	                     { \
	                         .silicon_ref = _ref, \
	                         .vendor      = "ti-cc3501e", \
	                         .base_caps   = 0u, \
	                         .priority    = 200, \
	                         .ops         = &_ops, \
	                         .probe       = NULL, \
	                     })

ALP_WIFI_CC3501E_REGISTER(cc3501e_e3, "alif:ensemble:e3");
ALP_WIFI_CC3501E_REGISTER(cc3501e_e4, "alif:ensemble:e4");
ALP_WIFI_CC3501E_REGISTER(cc3501e_e5, "alif:ensemble:e5");
ALP_WIFI_CC3501E_REGISTER(cc3501e_e6, "alif:ensemble:e6");
ALP_WIFI_CC3501E_REGISTER(cc3501e_e7, "alif:ensemble:e7");
ALP_WIFI_CC3501E_REGISTER(cc3501e_e8, "alif:ensemble:e8");
