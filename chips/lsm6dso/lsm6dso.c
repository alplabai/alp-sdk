/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * STMicroelectronics LSM6DSO 6-axis IMU driver.
 *
 * OS-agnostic: talks to the chip exclusively through the SDK's
 * <alp/peripheral.h> I2C surface, which routes to the right backend
 * (Zephyr i2c_*, vendor HAL on bare-metal, /dev/i2c-N on Yocto).
 */

#include <stddef.h>

#include "alp/chips/lsm6dso.h"

/* ------------------------------------------------------------------ */
/* Register map (DocID032086)                                         */
/* ------------------------------------------------------------------ */

#define REG_WHO_AM_I   0x0F
#define REG_CTRL1_XL   0x10
#define REG_CTRL2_G    0x11
#define REG_OUT_TEMP_L 0x20
#define REG_OUTX_L_G   0x22
#define REG_OUTX_L_A   0x28

static alp_status_t reg_write(lsm6dso_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read(lsm6dso_t *dev, uint8_t reg,
                             uint8_t *out, size_t len) {
    return alp_i2c_write_read(dev->bus, dev->addr,
                              &reg, 1, out, len);
}

static int16_t le16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t lsm6dso_init(lsm6dso_t *dev, alp_i2c_t *bus, uint8_t i2c_addr) {
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    dev->bus  = bus;
    dev->addr = i2c_addr;
    dev->accel_fs = LSM6DSO_ACCEL_FS_2G;
    dev->gyro_fs  = LSM6DSO_GYRO_FS_250_DPS;
    dev->initialised = false;

    uint8_t id = 0;
    alp_status_t s = lsm6dso_read_id(dev, &id);
    if (s != ALP_OK) return s;
    if (id != LSM6DSO_WHO_AM_I_VAL) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t lsm6dso_read_id(lsm6dso_t *dev, uint8_t *id_out) {
    if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
    return reg_read(dev, REG_WHO_AM_I, id_out, 1);
}

alp_status_t lsm6dso_set_accel(lsm6dso_t *dev,
                               lsm6dso_odr_t odr,
                               lsm6dso_accel_fs_t fs) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    /* CTRL1_XL: ODR_XL[7:4] | FS_XL[3:2] | LPF2_XL_EN[1] | reserved[0] */
    uint8_t v = (uint8_t)(((uint8_t)odr & 0x0Fu) << 4)
              | (uint8_t)(((uint8_t)fs  & 0x03u) << 2);
    alp_status_t s = reg_write(dev, REG_CTRL1_XL, v);
    if (s == ALP_OK) dev->accel_fs = fs;
    return s;
}

alp_status_t lsm6dso_set_gyro(lsm6dso_t *dev,
                              lsm6dso_odr_t odr,
                              lsm6dso_gyro_fs_t fs) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    /* CTRL2_G: ODR_G[7:4] | FS_G[3:2] | FS_125[1] | reserved[0] */
    uint8_t v = (uint8_t)(((uint8_t)odr & 0x0Fu) << 4)
              | (uint8_t)(((uint8_t)fs  & 0x03u) << 2);
    alp_status_t s = reg_write(dev, REG_CTRL2_G, v);
    if (s == ALP_OK) dev->gyro_fs = fs;
    return s;
}

alp_status_t lsm6dso_read_accel(lsm6dso_t *dev, lsm6dso_axes_t *out) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    uint8_t buf[6] = {0};
    alp_status_t s = reg_read(dev, REG_OUTX_L_A, buf, sizeof buf);
    if (s != ALP_OK) return s;
    out->x = le16(&buf[0]);
    out->y = le16(&buf[2]);
    out->z = le16(&buf[4]);
    return ALP_OK;
}

alp_status_t lsm6dso_read_gyro(lsm6dso_t *dev, lsm6dso_axes_t *out) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    uint8_t buf[6] = {0};
    alp_status_t s = reg_read(dev, REG_OUTX_L_G, buf, sizeof buf);
    if (s != ALP_OK) return s;
    out->x = le16(&buf[0]);
    out->y = le16(&buf[2]);
    out->z = le16(&buf[4]);
    return ALP_OK;
}

alp_status_t lsm6dso_read_temp(lsm6dso_t *dev, int16_t *temp_raw) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (temp_raw == NULL) return ALP_ERR_INVAL;
    uint8_t buf[2] = {0};
    alp_status_t s = reg_read(dev, REG_OUT_TEMP_L, buf, sizeof buf);
    if (s != ALP_OK) return s;
    *temp_raw = le16(buf);
    return ALP_OK;
}

void lsm6dso_deinit(lsm6dso_t *dev) {
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus = NULL;
}
