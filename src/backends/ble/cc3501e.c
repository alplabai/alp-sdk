/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E BLE backend for the portable <alp/ble.h> surface.
 *
 * The CC3501E firmware owns the BLE controller and NimBLE host.  The Alp SDK
 * backend wraps that firmware API so AEN applications can use alp_ble_* while
 * the chip-level cc3501e_* API remains available for diagnostics and console
 * commands.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "ble_ops.h"

#define CC3501E_BLE_ENABLE_TIMEOUT_MS 30000u
#define CC3501E_BLE_SCAN_TIMEOUT_MS   30000u
#define CC3501E_BLE_OP_TIMEOUT_MS     5000u

#define BLE_AD_TYPE_FLAGS       0x01u
#define BLE_AD_TYPE_UUID128_ALL 0x07u
#define BLE_AD_TYPE_NAME_COMP   0x09u
#define BLE_AD_FLAG_GENERAL     0x02u
#define BLE_AD_FLAG_NO_BREDR    0x04u
#define BLE_AD_MAX_LEN          31u

typedef struct {
	cc3501e_t *ctx;
	unsigned   refcount;
	bool       enabled;
} cc35_ble_be_t;

static cc35_ble_be_t _ble_be;

alp_status_t alp_ble_cc3501e_attach(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_INVAL;
	_ble_be.ctx = ctx;
	return ALP_OK;
}

static bool adv_put(uint8_t *ad, size_t *len, uint8_t type, const uint8_t *data, size_t data_len)
{
	size_t need = 2u + data_len;
	if (*len + need > BLE_AD_MAX_LEN || data_len > 0xffu) return false;
	ad[(*len)++] = (uint8_t)(data_len + 1u);
	ad[(*len)++] = type;
	if (data_len > 0u) {
		memcpy(&ad[*len], data, data_len);
		*len += data_len;
	}
	return true;
}

static alp_status_t pack_adv_data(const alp_ble_adv_config_t *cfg, uint8_t *ad, uint8_t *ad_len)
{
	size_t        len   = 0u;
	const uint8_t flags = BLE_AD_FLAG_GENERAL | BLE_AD_FLAG_NO_BREDR;

	if (!adv_put(ad, &len, BLE_AD_TYPE_FLAGS, &flags, sizeof(flags))) return ALP_ERR_INVAL;
	if (cfg->name != NULL && cfg->name[0] != '\0') {
		if (!adv_put(
		        ad, &len, BLE_AD_TYPE_NAME_COMP, (const uint8_t *)cfg->name, strlen(cfg->name))) {
			return ALP_ERR_INVAL;
		}
	}
	if (cfg->services != NULL && cfg->num_services > 0u) {
		size_t bytes = cfg->num_services * sizeof(alp_ble_uuid_t);
		if (!adv_put(ad, &len, BLE_AD_TYPE_UUID128_ALL, (const uint8_t *)cfg->services, bytes)) {
			return ALP_ERR_INVAL;
		}
	}
	*ad_len = (uint8_t)len;
	return ALP_OK;
}

static alp_status_t cc35_open(alp_ble_radio_state_t *state, alp_capabilities_t *caps_out)
{
	if (_ble_be.ctx == NULL) {
		caps_out->flags = 0u;
		return ALP_ERR_NOT_READY;
	}
	if (_ble_be.refcount == 0u || !_ble_be.enabled) {
		alp_status_t rc = cc3501e_ble_enable(_ble_be.ctx, CC3501E_BLE_ENABLE_TIMEOUT_MS);
		if (rc != ALP_OK) {
			caps_out->flags = 0u;
			return rc;
		}
		_ble_be.enabled = true;
	}
	_ble_be.refcount++;
	state->be_data  = &_ble_be;
	caps_out->flags = 0u;
	return ALP_OK;
}

static void cc35_close(alp_ble_radio_state_t *state)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL) return;
	if (be->refcount > 0u) be->refcount--;
	if (be->refcount == 0u && be->enabled) {
		(void)cc3501e_ble_disable(be->ctx, CC3501E_BLE_OP_TIMEOUT_MS);
		be->enabled = false;
	}
	state->be_data = NULL;
}

static alp_status_t cc35_advertise_start(alp_ble_radio_state_t      *state,
                                         const alp_ble_adv_config_t *cfg)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;

	uint8_t      ad[BLE_AD_MAX_LEN] = { 0 };
	uint8_t      ad_len             = 0u;
	alp_status_t rc                 = pack_adv_data(cfg, ad, &ad_len);
	if (rc != ALP_OK) return rc;

	return cc3501e_ble_adv_start(be->ctx,
	                             cfg->connectable,
	                             cfg->interval_min_ms,
	                             cfg->interval_max_ms,
	                             ad,
	                             ad_len,
	                             CC3501E_BLE_OP_TIMEOUT_MS);
}

static alp_status_t cc35_advertise_stop(alp_ble_radio_state_t *state)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	return cc3501e_ble_adv_stop(be->ctx, CC3501E_BLE_OP_TIMEOUT_MS);
}

static alp_status_t cc35_gatt_register_service(alp_ble_radio_state_t       *state,
                                               const alp_ble_service_def_t *def,
                                               alp_ble_attr_handle_t       *handles_out)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	if (def == NULL || handles_out == NULL) return ALP_ERR_INVAL;
	if (def->num_chars < 1u || def->num_chars > ALP_CC3501E_BLE_GATT_MAX_CHARS) {
		return ALP_ERR_INVAL;
	}

	/* Encode the BLE_GATT_REGISTER (0x38) request per the wire layout
	 * documented in <alp/protocol/cc3501e.h>: version | service_uuid[16] |
	 * num_chars, then per characteristic char_uuid[16] | properties(1) |
	 * initial_len(LE16) | initial_value[initial_len]. */
	uint8_t buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t  off = 0u;

	buf[off++] = ALP_CC3501E_BLE_GATT_REGISTER_VERSION;
	memcpy(&buf[off], def->service_uuid.b, sizeof(def->service_uuid.b));
	off += sizeof(def->service_uuid.b);
	buf[off++] = (uint8_t)def->num_chars;

	for (size_t i = 0u; i < def->num_chars; i++) {
		const alp_ble_char_def_t *c = &def->chars[i];
		if (c->initial_len > 0xFFFFu) return ALP_ERR_INVAL; /* wire field is LE16 */
		if (c->initial_value == NULL && c->initial_len > 0u) return ALP_ERR_INVAL;

		size_t need = off + sizeof(c->uuid.b) + 1u + 2u + c->initial_len;
		if (need > sizeof(buf)) return ALP_ERR_INVAL;

		memcpy(&buf[off], c->uuid.b, sizeof(c->uuid.b));
		off += sizeof(c->uuid.b);
		buf[off++] = c->properties;
		buf[off++] = (uint8_t)(c->initial_len & 0xFFu);
		buf[off++] = (uint8_t)((c->initial_len >> 8) & 0xFFu);
		if (c->initial_len > 0u) {
			memcpy(&buf[off], c->initial_value, c->initial_len);
			off += c->initial_len;
		}
	}

	uint16_t     handles[ALP_CC3501E_BLE_GATT_MAX_CHARS];
	size_t       num_handles = 0u;
	alp_status_t rc          = cc3501e_ble_gatt_register(be->ctx,
	                                                     buf,
	                                                     off,
	                                                     handles,
	                                                     ALP_CC3501E_BLE_GATT_MAX_CHARS,
	                                                     &num_handles,
	                                                     CC3501E_BLE_OP_TIMEOUT_MS);
	if (rc != ALP_OK) return rc;
	if (num_handles < def->num_chars) return ALP_ERR_IO; /* short reply -- shouldn't happen */

	for (size_t i = 0u; i < def->num_chars; i++) {
		handles_out[i] = (alp_ble_attr_handle_t)handles[i];
	}
	return ALP_OK;
}

static alp_status_t cc35_gatt_notify(alp_ble_radio_state_t *radio_state,
                                     alp_ble_conn_state_t  *conn_state,
                                     alp_ble_attr_handle_t  handle,
                                     const uint8_t         *payload,
                                     size_t                 len)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)radio_state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	(void)conn_state;
	return cc3501e_ble_gatt_notify(be->ctx, handle, payload, len, CC3501E_BLE_OP_TIMEOUT_MS);
}

static alp_status_t
cc35_scan_start(alp_ble_radio_state_t *state, bool active, alp_ble_scan_cb_t cb, void *user)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	(void)active;

	cc3501e_ble_scan_record_t recs[8];
	size_t                    count = 0u;
	alp_status_t              rc    = cc3501e_ble_scan(
	    be->ctx, recs, sizeof(recs) / sizeof(recs[0]), &count, CC3501E_BLE_SCAN_TIMEOUT_MS);
	if (rc != ALP_OK) return rc;

	for (size_t i = 0u; i < count; ++i) {
		alp_ble_scan_result_t r = {
			.addr.type = recs[i].addr_type,
			.rssi_dbm  = recs[i].rssi_dbm,
			.adv_type  = 0u,
			.adv_data  = (const uint8_t *)recs[i].name,
			.adv_len   = recs[i].name_len,
		};
		memcpy(r.addr.addr, recs[i].addr, sizeof(r.addr.addr));
		cb(&r, user);
	}
	return ALP_OK;
}

static alp_status_t cc35_scan_stop(alp_ble_radio_state_t *state)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	return cc3501e_ble_scan_stop(be->ctx, CC3501E_BLE_OP_TIMEOUT_MS);
}

static alp_status_t cc35_connect(alp_ble_radio_state_t *state,
                                 const alp_ble_addr_t  *peer,
                                 uint32_t               timeout_ms,
                                 alp_ble_conn_state_t  *conn_state_out)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;

	alp_status_t rc = cc3501e_ble_connect(be->ctx, peer->addr, peer->type, timeout_ms);
	if (rc != ALP_OK) return rc;
	conn_state_out->be_data = be;
	return ALP_OK;
}

static alp_status_t cc35_disconnect(alp_ble_conn_state_t *conn_state)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)conn_state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	conn_state->be_data = NULL;
	return cc3501e_ble_disconnect(be->ctx, CC3501E_BLE_OP_TIMEOUT_MS);
}

static alp_status_t cc35_gatt_read(alp_ble_conn_state_t *conn_state,
                                   alp_ble_attr_handle_t handle,
                                   uint8_t              *out,
                                   size_t                out_cap,
                                   size_t               *out_len,
                                   uint32_t              timeout_ms)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)conn_state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	return cc3501e_ble_gatt_read(be->ctx, handle, out, out_cap, out_len, timeout_ms);
}

static alp_status_t cc35_gatt_write(alp_ble_conn_state_t *conn_state,
                                    alp_ble_attr_handle_t handle,
                                    const uint8_t        *data,
                                    size_t                len,
                                    uint32_t              timeout_ms)
{
	cc35_ble_be_t *be = (cc35_ble_be_t *)conn_state->be_data;
	if (be == NULL || !be->enabled) return ALP_ERR_NOT_READY;
	return cc3501e_ble_gatt_write(be->ctx, handle, data, len, timeout_ms);
}

static const alp_ble_ops_t _ops = {
	.open                  = cc35_open,
	.advertise_start       = cc35_advertise_start,
	.advertise_stop        = cc35_advertise_stop,
	.gatt_register_service = cc35_gatt_register_service,
	.gatt_notify           = cc35_gatt_notify,
	.scan_start            = cc35_scan_start,
	.scan_stop             = cc35_scan_stop,
	.connect               = cc35_connect,
	.close                 = cc35_close,
	.disconnect            = cc35_disconnect,
	.gatt_read             = cc35_gatt_read,
	.gatt_write            = cc35_gatt_write,
};

#define ALP_BLE_CC3501E_REGISTER(_name, _ref) \
	ALP_BACKEND_REGISTER(ble, \
	                     _name, \
	                     { \
	                         .silicon_ref = _ref, \
	                         .vendor      = "ti-cc3501e", \
	                         .base_caps   = 0u, \
	                         .priority    = 200, \
	                         .ops         = &_ops, \
	                         .probe       = NULL, \
	                     })

ALP_BLE_CC3501E_REGISTER(cc3501e_e3, "alif:ensemble:e3");
ALP_BLE_CC3501E_REGISTER(cc3501e_e4, "alif:ensemble:e4");
ALP_BLE_CC3501E_REGISTER(cc3501e_e5, "alif:ensemble:e5");
ALP_BLE_CC3501E_REGISTER(cc3501e_e6, "alif:ensemble:e6");
ALP_BLE_CC3501E_REGISTER(cc3501e_e7, "alif:ensemble:e7");
ALP_BLE_CC3501E_REGISTER(cc3501e_e8, "alif:ensemble:e8");
