/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_spi dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_SPI_OPS_H
#define ALP_BACKENDS_SPI_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_spi_ops alp_spi_ops_t;

typedef struct alp_spi_backend_state {
    const struct device     *dev;        /* Zephyr backend device pointer */
    uint32_t                 bus_id;
    void                    *be_data;
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
    /* Zephyr SPI configuration -- populated by zephyr_drv backend */
    struct spi_config        zspi_cfg;
    struct spi_cs_control    cs_ctrl;
    struct gpio_dt_spec      cs_spec;    /* zeroed when no CS gpio resolved */
    bool                     cs_present;
};

#endif /* ALP_BACKENDS_SPI_OPS_H */
