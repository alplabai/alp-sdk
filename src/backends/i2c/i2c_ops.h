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
	/* Target (slave) mode -- optional.  Backends without target
	 * support leave both NULL; the dispatcher then fails
	 * alp_i2c_target_open with ALP_ERR_NOSUPPORT. */
	alp_status_t (*target_open)(const alp_i2c_target_config_t *cfg, alp_i2c_backend_state_t *state);
	void (*target_close)(alp_i2c_backend_state_t *state);
};

struct alp_i2c {
	alp_i2c_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

/* Lifecycle states for struct alp_i2c_target.  Driven atomically by
 * src/i2c_dispatch.c (see src/common/alp_slot_claim.h) so concurrent
 * closes race cleanly: exactly one caller unregisters the target and
 * returns the slot, instead of two closes tearing down (and a
 * concurrent open re-initialising) the same slot at once. */
#define ALP_I2C_TARGET_LC_UNOPENED 0u /* slot claimed but open unfinished / closed */
#define ALP_I2C_TARGET_LC_IDLE     1u
#define ALP_I2C_TARGET_LC_CLOSING  2u

struct alp_i2c_target {
	alp_i2c_backend_state_t state;
	const alp_backend_t    *backend;
	uint8_t                 lifecycle; /* ALP_I2C_TARGET_LC_*; atomic access only */
	bool                    in_use;
};

#endif /* ALP_BACKENDS_I2C_OPS_H */
