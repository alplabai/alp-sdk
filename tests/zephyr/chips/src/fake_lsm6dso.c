/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake LSM6DSO i2c-emul target.  Pre-populates WHO_AM_I = 0x6C and
 * echoes register writes back on subsequent reads — enough for the
 * chips ztest to exercise lsm6dso_init's WHO_AM_I check and the
 * CTRL1_XL / CTRL2_G register-protocol paths without a real device.
 */

#define DT_DRV_COMPAT alp_fake_lsm6dso

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

#define REG_WHO_AM_I 0x0F

struct fake_lsm6dso_data {
    uint8_t regs[256];
};

/* Singleton — the test only instantiates one fake LSM6DSO. */
static struct fake_lsm6dso_data *g_fake_lsm6dso;

static void seed_defaults(struct fake_lsm6dso_data *d) {
    memset(d->regs, 0, sizeof d->regs);
    d->regs[REG_WHO_AM_I] = 0x6C;
}

static int fake_lsm6dso_transfer(const struct emul *target,
                                 struct i2c_msg *msgs, int num_msgs,
                                 int addr) {
    (void)addr;
    struct fake_lsm6dso_data *d = target->data;

    if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
        /* Plain write: [reg, val0, val1, ...]. */
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
        /* Register read: [reg]; then read len bytes. */
        if (msgs[0].len < 1) return -EIO;
        const uint8_t reg0 = msgs[0].buf[0];
        for (uint32_t i = 0; i < msgs[1].len; i++) {
            msgs[1].buf[i] = d->regs[(uint8_t)(reg0 + i)];
        }
        return 0;
    }
    return -EIO;
}

static const struct i2c_emul_api fake_lsm6dso_api = {
    .transfer = fake_lsm6dso_transfer,
};

static int fake_lsm6dso_init(const struct emul *target,
                             const struct device *parent) {
    (void)parent;
    struct fake_lsm6dso_data *d = target->data;
    g_fake_lsm6dso = d;
    seed_defaults(d);
    return 0;
}

#define FAKE_LSM6DSO_DEFINE(n)                                           \
    static struct fake_lsm6dso_data fake_lsm6dso_data_##n;               \
    EMUL_DT_INST_DEFINE(n, fake_lsm6dso_init,                            \
                        &fake_lsm6dso_data_##n, NULL,                    \
                        &fake_lsm6dso_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_LSM6DSO_DEFINE)

FAKE_EMUL_DEV_SHIM()

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_lsm6dso_get_reg(uint8_t reg) {
    return g_fake_lsm6dso ? g_fake_lsm6dso->regs[reg] : 0u;
}

void fake_lsm6dso_set_reg(uint8_t reg, uint8_t val) {
    if (g_fake_lsm6dso) g_fake_lsm6dso->regs[reg] = val;
}

void fake_lsm6dso_reset(void) {
    if (g_fake_lsm6dso) seed_defaults(g_fake_lsm6dso);
}
