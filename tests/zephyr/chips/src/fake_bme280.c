/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake BME280 i2c-emul target.  Pre-populates CHIP_ID = 0x60 and
 * a synthetic but datasheet-shaped calibration block + a fixed raw
 * conversion result so the chips ztest can exercise
 * bme280_read_raw's bit-arithmetic and bme280_compensate's
 * coefficient-application path.
 *
 * The synthetic calibration values are chosen so the temperature
 * leg's compensation roundtrip stays inside the chip's working
 * range (-40 °C to +85 °C) for a wide span of raw inputs — small
 * enough that hand-arithmetic is tractable, but large enough to
 * exercise both signed and unsigned coefficient paths.
 */

#define DT_DRV_COMPAT alp_fake_bme280

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_CHIP_ID   0xD0
#define REG_CALIB_00  0x88
#define REG_CALIB_25  0xA1
#define REG_CALIB_26  0xE1
#define REG_PRESS_MSB 0xF7

struct fake_bme280_data {
    uint8_t regs[256];
};

static struct fake_bme280_data *g_fake_bme280;

/* Calibration coefficients are the canonical example from
 * BST-BME280-DS002 v1.6 §4.2.2.  Pairing them with the example raw
 * conversion below produces a known-good compensation result the
 * ztest can assert directly:  T = 25.08 °C, P ≈ 100653 Pa. */
static const uint8_t calib_block_1[24] = {
    0x70, 0x6B,    /* T1 =  27504 (u16) */
    0x43, 0x67,    /* T2 =  26435 (i16) */
    0x18, 0xFC,    /* T3 =  -1000 (i16) */
    0x7D, 0x8E,    /* P1 =  36477 (u16) */
    0x43, 0xD6,    /* P2 = -10685 (i16) */
    0xD0, 0x0B,    /* P3 =   3024 (i16) */
    0x27, 0x0B,    /* P4 =   2855 (i16) */
    0x8C, 0x00,    /* P5 =    140 (i16) */
    0xF9, 0xFF,    /* P6 =     -7 (i16) */
    0x8C, 0x3C,    /* P7 =  15500 (i16) */
    0xF8, 0xC6,    /* P8 = -14600 (i16) */
    0x70, 0x17,    /* P9 =   6000 (i16) */
};

static const uint8_t calib_h1 = 0x4B;       /* H1 = 75 */
static const uint8_t calib_block_2[7] = {
    0x6B, 0x01,    /* H2 (i16 LE) = 363 */
    0x00,          /* H3 = 0 */
    0x13, 0x05,    /* H4 (12-bit signed: E4 high8 | E5 low4)  */
    0x1E,          /* H5 (12-bit signed: E6 high8 | E5 high4) */
    0x1E,          /* H6 (i8) = 30 */
};

/* Datasheet example raw conversion: T_raw = 519888, P_raw = 415148.
 * Each MSB-first across 3 bytes. */
static const uint8_t raw_block[8] = {
    /* Pressure: 415148 = 0x655AC → MSB-first 20-bit packs as
     * 0x65 0x5A 0xC0 (low 4 bits unused). */
    0x65, 0x5A, 0xC0,
    /* Temperature: 519888 = 0x7EED0 → 0x7E 0xED 0x00. */
    0x7E, 0xED, 0x00,
    /* Humidity: 0x6FF0 (synthetic; no datasheet example). */
    0x6F, 0xF0,
};

static void seed_defaults(struct fake_bme280_data *d) {
    memset(d->regs, 0, sizeof d->regs);
    d->regs[REG_CHIP_ID] = 0x60;
    memcpy(&d->regs[REG_CALIB_00], calib_block_1, sizeof calib_block_1);
    d->regs[REG_CALIB_25] = calib_h1;
    memcpy(&d->regs[REG_CALIB_26], calib_block_2, sizeof calib_block_2);
    memcpy(&d->regs[REG_PRESS_MSB], raw_block, sizeof raw_block);
}

static int fake_bme280_transfer(const struct emul *target,
                                struct i2c_msg *msgs, int num_msgs,
                                int addr) {
    (void)addr;
    struct fake_bme280_data *d = target->data;

    if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
        if (msgs[0].len < 1) return -EIO;
        const uint8_t reg0 = msgs[0].buf[0];
        for (uint32_t i = 1; i < msgs[0].len; i++) {
            d->regs[(uint8_t)(reg0 + i - 1)] = msgs[0].buf[i];
        }
        return 0;
    }
    if (num_msgs == 2 &&
        (msgs[0].flags & I2C_MSG_READ) == 0 &&
        (msgs[1].flags & I2C_MSG_READ) != 0) {
        if (msgs[0].len < 1) return -EIO;
        const uint8_t reg0 = msgs[0].buf[0];
        for (uint32_t i = 0; i < msgs[1].len; i++) {
            msgs[1].buf[i] = d->regs[(uint8_t)(reg0 + i)];
        }
        return 0;
    }
    return -EIO;
}

static const struct i2c_emul_api fake_bme280_api = {
    .transfer = fake_bme280_transfer,
};

static int fake_bme280_init(const struct emul *target,
                            const struct device *parent) {
    (void)parent;
    struct fake_bme280_data *d = target->data;
    g_fake_bme280 = d;
    seed_defaults(d);
    return 0;
}

#define FAKE_BME280_DEFINE(n)                                            \
    static struct fake_bme280_data fake_bme280_data_##n;                 \
    EMUL_DT_INST_DEFINE(n, fake_bme280_init,                             \
                        &fake_bme280_data_##n, NULL,                    \
                        &fake_bme280_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_BME280_DEFINE)

/* The i2c_emul controller's emuls_<N> array references
 * __device_dts_ord_<N> for each child emul.  Without a paired
 * DEVICE_DT_INST_DEFINE the linker cannot resolve those symbols even
 * under Zephyr v4.4.  Register a no-op device to satisfy the reference. */
#define FAKE_DEV_DEFINE(n) \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_DEV_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_bme280_get_reg(uint8_t reg) {
    return g_fake_bme280 ? g_fake_bme280->regs[reg] : 0u;
}

void fake_bme280_set_reg(uint8_t reg, uint8_t val) {
    if (g_fake_bme280) g_fake_bme280->regs[reg] = val;
}

void fake_bme280_reset(void) {
    if (g_fake_bme280) seed_defaults(g_fake_bme280);
}
