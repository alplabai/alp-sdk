/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_i2s dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_I2S_OPS_H
#define ALP_BACKENDS_I2S_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>

typedef struct alp_i2s_ops alp_i2s_ops_t;

typedef struct alp_i2s_backend_state {
    void                    *dev;        /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
    uint32_t                 bus_id;
    void                    *be_data;    /* per-handle backend sidecar */
    const alp_i2s_ops_t     *ops;
} alp_i2s_backend_state_t;

struct alp_i2s_ops {
    alp_status_t (*open)(const alp_i2s_config_t *cfg,
                         alp_i2s_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*start)(alp_i2s_backend_state_t *state);
    alp_status_t (*stop)(alp_i2s_backend_state_t *state);
    alp_status_t (*write)(alp_i2s_backend_state_t *state,
                          const void *block, size_t bytes,
                          uint32_t timeout_ms);
    alp_status_t (*read)(alp_i2s_backend_state_t *state,
                         void *block, size_t bytes,
                         size_t *bytes_out,
                         uint32_t timeout_ms);
    void         (*close)(alp_i2s_backend_state_t *state);
};

/*
 * Portable handle layout.  Holds the caller's config snapshot + a
 * started flag so the dispatcher can short-circuit double-start/stop
 * without entering the backend; the Zephyr-specific k_mem_slab + slab
 * backing-store live in a sidecar inside src/backends/i2s/zephyr_drv.c
 * so non-Zephyr backends never touch a Zephyr type.
 */
struct alp_i2s {
    alp_i2s_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
    bool                     in_use;
    alp_i2s_config_t         cfg;        /* snapshot of caller's config */
    bool                     started;
};

#endif /* ALP_BACKENDS_I2S_OPS_H */
