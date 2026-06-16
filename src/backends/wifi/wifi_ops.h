/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_wifi dispatcher and per-backend
 * implementations.  The Wi-Fi station surface is single-instance per
 * E1M-conformant SoM (one radio per module), so unlike USB / BLE the
 * vtable carries one handle type only.  NOT a public header.
 */

#ifndef ALP_BACKENDS_WIFI_OPS_H
#define ALP_BACKENDS_WIFI_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

typedef struct alp_wifi_ops alp_wifi_ops_t;

typedef struct alp_wifi_backend_state {
	void                 *be_data;
	const alp_wifi_ops_t *ops;
} alp_wifi_backend_state_t;

struct alp_wifi_ops {
	alp_status_t (*open)(alp_wifi_backend_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*connect)(alp_wifi_backend_state_t *state, const alp_wifi_credentials_t *creds,
	                        uint32_t timeout_ms);
	alp_status_t (*disconnect)(alp_wifi_backend_state_t *state);
	void (*close)(alp_wifi_backend_state_t *state);
};

struct alp_wifi {
	alp_wifi_backend_state_t state;
	const alp_backend_t     *backend;
	alp_capabilities_t       cached_caps;
	bool                     in_use;
};

#endif /* ALP_BACKENDS_WIFI_OPS_H */
