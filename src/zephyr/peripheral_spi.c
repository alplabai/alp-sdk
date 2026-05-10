/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/peripheral.h> — SPI.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_SPI_DEV_OR_NULL(idx)                                                \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_spi, idx))),                \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_spi, idx)))), (NULL))

static const struct device *const alp_spi_devs[] = {
    ALP_SPI_DEV_OR_NULL(0),
    ALP_SPI_DEV_OR_NULL(1),
    ALP_SPI_DEV_OR_NULL(2),
    ALP_SPI_DEV_OR_NULL(3),
    ALP_SPI_DEV_OR_NULL(4),
    ALP_SPI_DEV_OR_NULL(5),
    ALP_SPI_DEV_OR_NULL(6),
    ALP_SPI_DEV_OR_NULL(7),
};

/* Resolve a chip-select gpio_dt_spec from the alp,pin-array node by index.
 * Mirrors what peripheral_gpio.c does — see that file for the binding. */
extern bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out);

static uint16_t to_spi_op(const alp_spi_config_t *cfg) {
    uint16_t op = SPI_WORD_SET(cfg->bits_per_word ? cfg->bits_per_word : 8);
    op |= SPI_OP_MODE_MASTER;
    op |= SPI_TRANSFER_MSB;          /* SPI_MODE_x already covers CPOL/CPHA */
    if (cfg->mode & 0x2u) op |= SPI_MODE_CPOL;
    if (cfg->mode & 0x1u) op |= SPI_MODE_CPHA;
    return op;
}

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:        return ALP_OK;
    case -EINVAL:  return ALP_ERR_INVAL;
    case -EBUSY:   return ALP_ERR_BUSY;
    case -ETIMEDOUT: return ALP_ERR_TIMEOUT;
    case -EIO:     return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:  return ALP_ERR_NOSUPPORT;
    default:       return ALP_ERR_IO;
    }
}

#define ALP_SPI_NO_CS  0xFFFFFFFFu

alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ARRAY_SIZE(alp_spi_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ALP_SOC_SPI_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_spi_devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_spi *h = alp_z_spi_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->bus_id = cfg->bus_id;
    h->dev    = dev;
    h->cfg    = *cfg;

    h->zspi_cfg.frequency = cfg->freq_hz ? cfg->freq_hz : 1000000;
    h->zspi_cfg.operation = to_spi_op(cfg);
    h->zspi_cfg.slave     = 0;

    if (cfg->cs_pin_id != ALP_SPI_NO_CS &&
        alp_z_gpio_resolve(cfg->cs_pin_id, &h->cs_spec)) {
        if (!device_is_ready(h->cs_spec.port)) {
            alp_z_set_last_error(ALP_ERR_NOT_READY);
            alp_z_spi_pool_release(h);
            return NULL;
        }
        gpio_pin_configure_dt(&h->cs_spec, GPIO_OUTPUT_INACTIVE);
        h->cs_ctrl.gpio = h->cs_spec;
        h->cs_ctrl.delay = 0;
        h->zspi_cfg.cs = h->cs_ctrl;
        h->cs_present = true;
    }

    return h;
}

void alp_spi_close(alp_spi_t *bus) {
    alp_z_spi_pool_release(bus);
}

alp_status_t alp_spi_transceive(alp_spi_t *bus,
                                const uint8_t *tx, uint8_t *rx,
                                size_t len) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if (len == 0) return ALP_OK;

    struct spi_buf tx_buf = { .buf = (void *)tx, .len = (tx != NULL) ? len : 0 };
    struct spi_buf rx_buf = { .buf = rx,         .len = (rx != NULL) ? len : 0 };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

    int err = spi_transceive(bus->dev, &bus->zspi_cfg,
                             (tx != NULL) ? &tx_set : NULL,
                             (rx != NULL) ? &rx_set : NULL);
    return errno_to_alp(err);
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len) {
    return alp_spi_transceive(bus, tx, NULL, len);
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len) {
    return alp_spi_transceive(bus, NULL, rx, len);
}
