/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * STMicroelectronics LIS2DW12 3-axis accelerometer driver.  See header.
 */

#include <stddef.h>

#include "alp/chips/lis2dw12.h"

/* ------------------------------------------------------------------ */
/* Register map (DocID030334)                                         */
/* ------------------------------------------------------------------ */

#define REG_OUT_T_L    0x0D
#define REG_WHO_AM_I   0x0F
#define REG_CTRL1      0x20   /* ODR[7:4] | MODE[3:2] | LP_MODE[1:0]. */
#define REG_CTRL2      0x21
#define REG_CTRL6      0x25   /* BW_FILT[7:6] | FS[5:4] | FDS[3] | LOW_NOISE[2] | reserved. */
#define REG_OUT_X_L    0x28

static alp_status_t reg_write(lis2dw12_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read(lis2dw12_t *dev, uint8_t reg,
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

alp_status_t lis2dw12_init(lis2dw12_t *dev, alp_i2c_t *bus, uint8_t i2c_addr) {
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    dev->bus  = bus;
    dev->addr = i2c_addr;
    dev->fs   = LIS2DW12_FS_2G;
    dev->mode = LIS2DW12_MODE_LOW_POWER_12BIT;
    dev->initialised = false;

    uint8_t id = 0;
    alp_status_t s = lis2dw12_read_id(dev, &id);
    if (s != ALP_OK) return s;
    if (id != LIS2DW12_WHO_AM_I_VAL) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t lis2dw12_read_id(lis2dw12_t *dev, uint8_t *id_out) {
    if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
    return reg_read(dev, REG_WHO_AM_I, id_out, 1);
}

alp_status_t lis2dw12_set_accel(lis2dw12_t *dev,
                                lis2dw12_odr_t odr,
                                lis2dw12_fs_t fs,
                                lis2dw12_mode_t mode) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

    /* CTRL1: ODR[7:4] | MODE[3:2] | LP_MODE[1:0].  We split the four
     * lis2dw12_mode_t presets into the two-field encoding the chip
     * uses internally. */
    uint8_t mode_bits, lp_bits;
    switch (mode) {
        case LIS2DW12_MODE_LOW_POWER_12BIT:  mode_bits = 0x0; lp_bits = 0x0; break;
        case LIS2DW12_MODE_LOW_POWER_14BIT:  mode_bits = 0x0; lp_bits = 0x1; break;
        case LIS2DW12_MODE_HIGH_PERF_14BIT:  mode_bits = 0x1; lp_bits = 0x0; break;
        case LIS2DW12_MODE_SINGLE_SHOT:      mode_bits = 0x2; lp_bits = 0x0; break;
        default: return ALP_ERR_INVAL;
    }
    const uint8_t ctrl1 = (uint8_t)((((uint8_t)odr  & 0x0Fu) << 4)
                                  | ((mode_bits     & 0x03u) << 2)
                                  |  (lp_bits       & 0x03u));
    alp_status_t s = reg_write(dev, REG_CTRL1, ctrl1);
    if (s != ALP_OK) return s;

    /* CTRL6: keep BW_FILT default (00 = ODR/2), set FS, leave the
     * rest at reset value. */
    const uint8_t ctrl6 = (uint8_t)(((uint8_t)fs & 0x03u) << 4);
    s = reg_write(dev, REG_CTRL6, ctrl6);
    if (s == ALP_OK) {
        dev->fs = fs;
        dev->mode = mode;
    }
    return s;
}

alp_status_t lis2dw12_read_accel(lis2dw12_t *dev, lis2dw12_axes_t *out) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    uint8_t buf[6] = {0};
    alp_status_t s = reg_read(dev, REG_OUT_X_L, buf, sizeof buf);
    if (s != ALP_OK) return s;
    out->x = le16(&buf[0]);
    out->y = le16(&buf[2]);
    out->z = le16(&buf[4]);
    return ALP_OK;
}

alp_status_t lis2dw12_read_temp(lis2dw12_t *dev, int16_t *temp_raw) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (temp_raw == NULL) return ALP_ERR_INVAL;
    uint8_t buf[2] = {0};
    alp_status_t s = reg_read(dev, REG_OUT_T_L, buf, sizeof buf);
    if (s != ALP_OK) return s;
    *temp_raw = le16(buf);
    return ALP_OK;
}

void lis2dw12_deinit(lis2dw12_t *dev) {
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus = NULL;
}
