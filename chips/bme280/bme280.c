/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BME280 driver (T/H/P).  See header.
 *
 * Compensation arithmetic transcribed verbatim from BST-BME280-DS002
 * v1.6 §4.2.3 (integer-only forms).  The Bosch reference formulas are
 * deliberately preserved bit-for-bit so on-target results match
 * datasheet expectations.
 */

#include <stddef.h>

#include "alp/chips/bme280.h"

/* ------------------------------------------------------------------ */
/* Register map (BST-BME280-DS002 v1.6 §5)                             */
/* ------------------------------------------------------------------ */

#define REG_CHIP_ID     0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_STATUS      0xF3
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7   /* Burst start: P[3] | T[3] | H[2] = 8 bytes. */
#define REG_CALIB_00    0x88   /* Block 1: T1..T3 (3×u16 LE) | P1..P9 (9×i16 LE) = 26 B. */
#define REG_CALIB_25    0xA1   /* H1 (1 B). */
#define REG_CALIB_26    0xE1   /* Block 2: H2..H6 packed across 7 B. */

#define RESET_MAGIC     0xB6

static alp_status_t reg_write(bme280_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read(bme280_t *dev, uint8_t reg,
                             uint8_t *out, size_t len) {
    return alp_i2c_write_read(dev->bus, dev->addr,
                              &reg, 1, out, len);
}

static uint16_t le16u(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t le16s(const uint8_t *p) {
    return (int16_t)le16u(p);
}

static alp_status_t load_calibration(bme280_t *dev) {
    uint8_t blk1[26] = {0};
    alp_status_t s = reg_read(dev, REG_CALIB_00, blk1, sizeof blk1);
    if (s != ALP_OK) return s;

    uint8_t h1 = 0;
    s = reg_read(dev, REG_CALIB_25, &h1, 1);
    if (s != ALP_OK) return s;

    uint8_t blk2[7] = {0};
    s = reg_read(dev, REG_CALIB_26, blk2, sizeof blk2);
    if (s != ALP_OK) return s;

    bme280_calib_t *c = &dev->calib;
    c->T1 = le16u(&blk1[0]);
    c->T2 = le16s(&blk1[2]);
    c->T3 = le16s(&blk1[4]);
    c->P1 = le16u(&blk1[6]);
    c->P2 = le16s(&blk1[8]);
    c->P3 = le16s(&blk1[10]);
    c->P4 = le16s(&blk1[12]);
    c->P5 = le16s(&blk1[14]);
    c->P6 = le16s(&blk1[16]);
    c->P7 = le16s(&blk1[18]);
    c->P8 = le16s(&blk1[20]);
    c->P9 = le16s(&blk1[22]);
    c->H1 = h1;
    c->H2 = le16s(&blk2[0]);
    c->H3 = blk2[2];
    /* H4: [E4][7:0] << 4 | [E5][3:0]   — 12-bit signed. */
    c->H4 = (int16_t)(((int16_t)(int8_t)blk2[3] << 4) | (blk2[4] & 0x0F));
    /* H5: [E6][7:0] << 4 | [E5][7:4]   — 12-bit signed. */
    c->H5 = (int16_t)(((int16_t)(int8_t)blk2[5] << 4) | ((blk2[4] >> 4) & 0x0F));
    c->H6 = (int8_t)blk2[6];
    return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t bme280_init(bme280_t *dev, alp_i2c_t *bus, uint8_t i2c_addr) {
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    dev->bus  = bus;
    dev->addr = i2c_addr;
    dev->t_fine = 0;
    dev->initialised = false;

    uint8_t id = 0;
    alp_status_t s = bme280_read_id(dev, &id);
    if (s != ALP_OK) return s;
    if (id != BME280_CHIP_ID) return ALP_ERR_IO;

    s = load_calibration(dev);
    if (s != ALP_OK) return s;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t bme280_read_id(bme280_t *dev, uint8_t *id_out) {
    if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
    return reg_read(dev, REG_CHIP_ID, id_out, 1);
}

alp_status_t bme280_set_sampling(bme280_t *dev,
                                 bme280_oversampling_t t_os,
                                 bme280_oversampling_t p_os,
                                 bme280_oversampling_t h_os,
                                 bme280_mode_t mode,
                                 bme280_standby_t standby,
                                 bme280_filter_t filter) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

    /* Per datasheet §5.4.3: CTRL_HUM must be written first; CTRL_MEAS
     * latches it on the next CTRL_MEAS write. */
    alp_status_t s = reg_write(dev, REG_CTRL_HUM,
                               (uint8_t)((uint8_t)h_os & 0x07u));
    if (s != ALP_OK) return s;

    /* CONFIG: t_sb[7:5] | filter[4:2] | spi3w_en[0] (0 in I2C-only mode). */
    const uint8_t cfg = (uint8_t)((((uint8_t)standby & 0x07u) << 5)
                                | (((uint8_t)filter  & 0x07u) << 2));
    s = reg_write(dev, REG_CONFIG, cfg);
    if (s != ALP_OK) return s;

    /* CTRL_MEAS: osrs_t[7:5] | osrs_p[4:2] | mode[1:0]. */
    const uint8_t cm = (uint8_t)((((uint8_t)t_os & 0x07u) << 5)
                               | (((uint8_t)p_os & 0x07u) << 2)
                               |  ((uint8_t)mode & 0x03u));
    return reg_write(dev, REG_CTRL_MEAS, cm);
}

alp_status_t bme280_read_raw(bme280_t *dev, bme280_raw_t *out) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    uint8_t buf[8] = {0};
    alp_status_t s = reg_read(dev, REG_PRESS_MSB, buf, sizeof buf);
    if (s != ALP_OK) return s;

    /* Pressure: 20-bit, MSB-first across [F7][F8][F9][7:4]. */
    out->pressure_raw = (int32_t)((((uint32_t)buf[0]) << 12)
                                | (((uint32_t)buf[1]) << 4)
                                |  ((uint32_t)buf[2]  >> 4));
    /* Temperature: 20-bit, MSB-first across [FA][FB][FC][7:4]. */
    out->temperature_raw = (int32_t)((((uint32_t)buf[3]) << 12)
                                   | (((uint32_t)buf[4]) << 4)
                                   |  ((uint32_t)buf[5]  >> 4));
    /* Humidity: 16-bit, MSB-first across [FD][FE]. */
    out->humidity_raw = (uint32_t)((((uint32_t)buf[6]) << 8)
                                 |  ((uint32_t)buf[7]));
    return ALP_OK;
}

alp_status_t bme280_compensate(bme280_t *dev,
                               const bme280_raw_t *raw,
                               bme280_compensated_t *out) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (raw == NULL || out == NULL) return ALP_ERR_INVAL;

    const bme280_calib_t *c = &dev->calib;

    /* --- Temperature (BST-BME280-DS002 §4.2.3, integer form) --- */
    int32_t adc_T = raw->temperature_raw;
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)c->T1 << 1)))
                    * ((int32_t)c->T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)c->T1))
                    * ((adc_T >> 4) - ((int32_t)c->T1))) >> 12)
                    * ((int32_t)c->T3)) >> 14;
    dev->t_fine = var1 + var2;
    out->temperature_c100 = (dev->t_fine * 5 + 128) >> 8;

    /* --- Pressure (Pa) --- */
    int32_t adc_P = raw->pressure_raw;
    int64_t pv1 = ((int64_t)dev->t_fine) - 128000;
    int64_t pv2 = pv1 * pv1 * (int64_t)c->P6;
    pv2 = pv2 + ((pv1 * (int64_t)c->P5) << 17);
    pv2 = pv2 + (((int64_t)c->P4) << 35);
    pv1 = ((pv1 * pv1 * (int64_t)c->P3) >> 8)
        + ((pv1 * (int64_t)c->P2) << 12);
    pv1 = ((((int64_t)1) << 47) + pv1) * ((int64_t)c->P1) >> 33;
    if (pv1 == 0) {
        out->pressure_pa = 0;   /* Avoid /0 — reset on next sample. */
    } else {
        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - pv2) * 3125) / pv1;
        pv1 = (((int64_t)c->P9) * (p >> 13) * (p >> 13)) >> 25;
        pv2 = (((int64_t)c->P8) * p) >> 19;
        p = ((p + pv1 + pv2) >> 8) + (((int64_t)c->P7) << 4);
        out->pressure_pa = (uint32_t)(p / 256);
    }

    /* --- Humidity (Q22.10 %RH) --- */
    int32_t adc_H = (int32_t)raw->humidity_raw;
    int32_t hv = dev->t_fine - 76800;
    hv = (((((adc_H << 14) - (((int32_t)c->H4) << 20)
              - (((int32_t)c->H5) * hv)) + 16384) >> 15)
            * (((((((hv * ((int32_t)c->H6)) >> 10)
                    * (((hv * ((int32_t)c->H3)) >> 11) + 32768)) >> 10)
                  + 2097152) * ((int32_t)c->H2) + 8192) >> 14));
    hv = hv - (((((hv >> 15) * (hv >> 15)) >> 7) * ((int32_t)c->H1)) >> 4);
    if (hv < 0) hv = 0;
    if (hv > 419430400) hv = 419430400;
    out->humidity_milli_pct = (uint32_t)(hv >> 12);
    return ALP_OK;
}

alp_status_t bme280_soft_reset(bme280_t *dev) {
    if (dev == NULL || dev->bus == NULL) return ALP_ERR_INVAL;
    return reg_write(dev, REG_RESET, RESET_MAGIC);
}

void bme280_deinit(bme280_t *dev) {
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus = NULL;
}
