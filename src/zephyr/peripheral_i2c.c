/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/peripheral.h> — I2C.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "alp/soc_caps.h"
#include "handles.h"

/* Resolve the studio-side bus_id to a Zephyr device pointer via the
 * `alp-i2cN` devicetree alias.  Slots without an alias resolve to NULL. */
#define ALP_I2C_DEV_OR_NULL(idx)                                                \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_i2c, idx))),                \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2c, idx)))), (NULL))

static const struct device *const alp_i2c_devs[] = {
    ALP_I2C_DEV_OR_NULL(0),
    ALP_I2C_DEV_OR_NULL(1),
    ALP_I2C_DEV_OR_NULL(2),
    ALP_I2C_DEV_OR_NULL(3),
    ALP_I2C_DEV_OR_NULL(4),
    ALP_I2C_DEV_OR_NULL(5),
    ALP_I2C_DEV_OR_NULL(6),
    ALP_I2C_DEV_OR_NULL(7),
};

static uint32_t alp_to_zephyr_bitrate_flags(uint32_t bitrate_hz) {
    if (bitrate_hz >= 1000000) return I2C_SPEED_SET(I2C_SPEED_FAST_PLUS);
    if (bitrate_hz >=  400000) return I2C_SPEED_SET(I2C_SPEED_FAST);
    return I2C_SPEED_SET(I2C_SPEED_STANDARD);
}

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:        return ALP_OK;
    case -EINVAL:  return ALP_ERR_INVAL;
    case -EBUSY:   return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT: return ALP_ERR_TIMEOUT;
    case -EIO:     return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:  return ALP_ERR_NOSUPPORT;
    default:       return ALP_ERR_IO;
    }
}

alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ARRAY_SIZE(alp_i2c_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ALP_SOC_I2C_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_i2c_devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_i2c *h = alp_z_i2c_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    uint32_t flags = I2C_MODE_CONTROLLER | alp_to_zephyr_bitrate_flags(cfg->bitrate_hz);
    int err = i2c_configure(dev, flags);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        alp_z_i2c_pool_release(h);
        return NULL;
    }

    h->bus_id = cfg->bus_id;
    h->dev    = dev;
    h->cfg    = *cfg;
    return h;
}

void alp_i2c_close(alp_i2c_t *bus) {
    alp_z_i2c_pool_release(bus);
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr,
                           const uint8_t *data, size_t len) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    return errno_to_alp(i2c_write(bus->dev, data, len, addr));
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr,
                          uint8_t *data, size_t len) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    return errno_to_alp(i2c_read(bus->dev, data, len, addr));
}

alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr,
                                const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if ((wdata == NULL && wlen > 0) || (rdata == NULL && rlen > 0)) {
        return ALP_ERR_INVAL;
    }
    return errno_to_alp(i2c_write_read(bus->dev, addr,
                                       wdata, wlen,
                                       rdata, rlen));
}
