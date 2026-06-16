/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_i2c dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_I2C_OPS_H
#define ALP_BACKENDS_I2C_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_i2c_ops alp_i2c_ops_t;

typedef struct alp_i2c_backend_state {
	void                *dev; /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t             bus_id;
	void                *be_data;
	const alp_i2c_ops_t *ops;
} alp_i2c_backend_state_t;

struct alp_i2c_ops {
	alp_status_t (*open)(const alp_i2c_config_t  *cfg,
	                     alp_i2c_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*write)(alp_i2c_backend_state_t *state,
	                      uint8_t                  addr,
	                      const uint8_t           *data,
	                      size_t                   len);
	alp_status_t (*read)(alp_i2c_backend_state_t *state, uint8_t addr, uint8_t *data, size_t len);
	alp_status_t (*write_read)(alp_i2c_backend_state_t *state,
	                           uint8_t                  addr,
	                           const uint8_t           *wdata,
	                           size_t                   wlen,
	                           uint8_t                 *rdata,
	                           size_t                   rlen);
	void (*close)(alp_i2c_backend_state_t *state);
};

struct alp_i2c {
	alp_i2c_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_I2C_OPS_H */
