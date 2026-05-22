/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr spi_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/spi_dispatch.c; the
 * backend's open fills state->dev, builds the spi_config, and
 * resolves the chip-select GPIO if one is specified.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "spi_ops.h"

#define ALP_SPI_DEV_OR_NULL(idx) \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_spi, idx))), \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_spi, idx)))), (NULL))

static const struct device *const _devs[] = {
    ALP_SPI_DEV_OR_NULL(0),
    ALP_SPI_DEV_OR_NULL(1),
    ALP_SPI_DEV_OR_NULL(2),
    ALP_SPI_DEV_OR_NULL(3),
    ALP_SPI_DEV_OR_NULL(4),
    ALP_SPI_DEV_OR_NULL(5),
    ALP_SPI_DEV_OR_NULL(6),
    ALP_SPI_DEV_OR_NULL(7),
};

/* Re-use the GPIO resolution helper from peripheral_gpio.c. */
extern bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out);

#define ALP_SPI_NO_CS  0xFFFFFFFFu

static uint16_t _to_spi_op(const alp_spi_config_t *cfg) {
    uint16_t op = SPI_WORD_SET(cfg->bits_per_word ? cfg->bits_per_word : 8);
    op |= SPI_OP_MODE_MASTER;
    op |= SPI_TRANSFER_MSB;
    if (cfg->mode & 0x2u) op |= SPI_MODE_CPOL;
    if (cfg->mode & 0x1u) op |= SPI_MODE_CPHA;
    return op;
}

static alp_status_t _errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ETIMEDOUT:  return ALP_ERR_TIMEOUT;
    case -EIO:        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

static alp_status_t z_open(const alp_spi_config_t *cfg,
                           alp_spi_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
    if (cfg->bus_id >= ALP_SOC_SPI_COUNT) return ALP_ERR_OUT_OF_RANGE;
    const struct device *dev = _devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

    /* Recover the full handle via container_of so we can populate the
     * Zephyr SPI config fields that transceive will need. */
    struct alp_spi *h = CONTAINER_OF(st, struct alp_spi, state);

    h->zspi_cfg.frequency = cfg->freq_hz ? cfg->freq_hz : 1000000u;
    h->zspi_cfg.operation = _to_spi_op(cfg);
    h->zspi_cfg.slave     = 0;

    if (cfg->cs_pin_id != ALP_SPI_NO_CS &&
        alp_z_gpio_resolve(cfg->cs_pin_id, &h->cs_spec)) {
        if (!device_is_ready(h->cs_spec.port)) {
            return ALP_ERR_NOT_READY;
        }
        gpio_pin_configure_dt(&h->cs_spec, GPIO_OUTPUT_INACTIVE);
        h->cs_ctrl.gpio  = h->cs_spec;
        h->cs_ctrl.delay = 0;
        h->zspi_cfg.cs   = h->cs_ctrl;
        h->cs_present    = true;
    }

    st->dev    = dev;
    st->bus_id = cfg->bus_id;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_transceive(alp_spi_backend_state_t *st,
                                 const uint8_t *tx, uint8_t *rx,
                                 size_t len) {
    struct alp_spi *h = CONTAINER_OF(st, struct alp_spi, state);

    struct spi_buf tx_buf = { .buf = (void *)tx, .len = (tx != NULL) ? len : 0 };
    struct spi_buf rx_buf = { .buf = rx,         .len = (rx != NULL) ? len : 0 };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

    int err = spi_transceive(st->dev, &h->zspi_cfg,
                             (tx != NULL) ? &tx_set : NULL,
                             (rx != NULL) ? &rx_set : NULL);
    return _errno_to_alp(err);
}

static const alp_spi_ops_t _ops = {
    .open       = z_open,
    .transceive = z_transceive,
    .close      = NULL,     /* no teardown needed for spi_transceive */
};

ALP_BACKEND_REGISTER(spi, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
