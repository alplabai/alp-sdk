/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_spi dispatcher and per-backend
 * implementations.  NOT a public header.
 *
 * Zephyr leakage: state->dev is typed void* and the per-handle
 * Zephyr SPI config (spi_config + spi_cs_control + gpio_dt_spec)
 * lives in a backend-private sidecar reached via state.be_data
 * inside src/backends/spi/zephyr_drv.c.  This keeps the portable
 * dispatcher TU and the struct alp_spi layout free of
 * <zephyr/device.h>, <zephyr/drivers/gpio.h>, and
 * <zephyr/drivers/spi.h>.
 */

#ifndef ALP_BACKENDS_SPI_OPS_H
#define ALP_BACKENDS_SPI_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_spi_ops alp_spi_ops_t;

typedef struct alp_spi_backend_state {
    void                    *dev;        /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
    uint32_t                 bus_id;
    void                    *be_data;    /* per-handle backend sidecar
                                          * (Zephyr backend stashes spi_config +
                                          * cs_ctrl + cs_spec + cs_present here) */
    const alp_spi_ops_t     *ops;
} alp_spi_backend_state_t;

struct alp_spi_ops {
    alp_status_t (*open)(const alp_spi_config_t *cfg,
                         alp_spi_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*transceive)(alp_spi_backend_state_t *state,
                               const uint8_t *tx, uint8_t *rx,
                               size_t len);
    void         (*close)(alp_spi_backend_state_t *state);
};

struct alp_spi {
    alp_spi_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
    bool                     in_use;
};

#endif /* ALP_BACKENDS_SPI_OPS_H */
