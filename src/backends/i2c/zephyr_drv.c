/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr i2c_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/i2c_dispatch.c; the
 * backend's open fills state->dev and configures the bus speed.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "i2c_ops.h"

#define ALP_I2C_DEV_OR_NULL(idx) \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_i2c, idx))), \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2c, idx)))), (NULL))

static const struct device *const _devs[] = {
    ALP_I2C_DEV_OR_NULL(0),
    ALP_I2C_DEV_OR_NULL(1),
    ALP_I2C_DEV_OR_NULL(2),
    ALP_I2C_DEV_OR_NULL(3),
    ALP_I2C_DEV_OR_NULL(4),
    ALP_I2C_DEV_OR_NULL(5),
    ALP_I2C_DEV_OR_NULL(6),
    ALP_I2C_DEV_OR_NULL(7),
};

static uint32_t _alp_to_zephyr_bitrate_flags(uint32_t bitrate_hz) {
    if (bitrate_hz >= 1000000) return I2C_SPEED_SET(I2C_SPEED_FAST_PLUS);
    if (bitrate_hz >=  400000) return I2C_SPEED_SET(I2C_SPEED_FAST);
    return I2C_SPEED_SET(I2C_SPEED_STANDARD);
}

static alp_status_t _errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:  return ALP_ERR_TIMEOUT;
    case -EIO:        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

static alp_status_t z_open(const alp_i2c_config_t *cfg,
                           alp_i2c_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
    if (cfg->bus_id >= ALP_SOC_I2C_COUNT) return ALP_ERR_OUT_OF_RANGE;
    const struct device *dev = _devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
    uint32_t flags = I2C_MODE_CONTROLLER | _alp_to_zephyr_bitrate_flags(cfg->bitrate_hz);
    int err = i2c_configure(dev, flags);
    if (err != 0) return _errno_to_alp(err);
    st->dev    = dev;
    st->bus_id = cfg->bus_id;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_write(alp_i2c_backend_state_t *st,
                            uint8_t addr,
                            const uint8_t *data, size_t len) {
    return _errno_to_alp(i2c_write(st->dev, data, len, addr));
}

static alp_status_t z_read(alp_i2c_backend_state_t *st,
                           uint8_t addr,
                           uint8_t *data, size_t len) {
    return _errno_to_alp(i2c_read(st->dev, data, len, addr));
}

static alp_status_t z_write_read(alp_i2c_backend_state_t *st,
                                 uint8_t addr,
                                 const uint8_t *wdata, size_t wlen,
                                 uint8_t *rdata, size_t rlen) {
    return _errno_to_alp(i2c_write_read(st->dev, addr, wdata, wlen, rdata, rlen));
}

static const alp_i2c_ops_t _ops = {
    .open       = z_open,
    .write      = z_write,
    .read       = z_read,
    .write_read = z_write_read,
    .close      = NULL,     /* no teardown needed for i2c_configure */
};

ALP_BACKEND_REGISTER(i2c, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
