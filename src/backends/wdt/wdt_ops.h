/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_wdt dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_WDT_OPS_H
#define ALP_BACKENDS_WDT_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/wdt.h>

typedef struct alp_wdt_ops alp_wdt_ops_t;

typedef struct alp_wdt_backend_state {
	void                *dev; /* opaque backend device pointer
                                               * (const struct device * on Zephyr;
                                               * kept void* so the portable handle
                                               * does not pull in <zephyr/device.h>) */
	uint32_t             wdt_id;
	int                  channel_id; /* wdt_install_timeout return code */
	alp_wdt_config_t     cfg;
	void                *be_data;
	const alp_wdt_ops_t *ops;
} alp_wdt_backend_state_t;

struct alp_wdt_ops {
	alp_status_t (*open)(uint32_t wdt_id, const alp_wdt_config_t *cfg,
	                     alp_wdt_backend_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*feed)(alp_wdt_backend_state_t *state);
	alp_status_t (*disable)(alp_wdt_backend_state_t *state);
	void (*close)(alp_wdt_backend_state_t *state);
};

struct alp_wdt {
	alp_wdt_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_WDT_OPS_H */
