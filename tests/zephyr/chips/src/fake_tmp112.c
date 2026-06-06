/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI TMP112.  Four 16-bit registers, big-endian on the wire:
 * reads return [MSB, LSB] of the pointed register; writes take
 * [reg, MSB, LSB].  Seeds the datasheet power-on CONF (0x60A0 --
 * R1:R0 read 11, which the driver's probe fingerprints).
 */
#define DT_DRV_COMPAT alp_fake_tmp112

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_tmp112_data {
    uint16_t regs[4];
    uint8_t  ptr;
};

static struct fake_tmp112_data *g_tmp112;

uint16_t                        fake_tmp112_get_reg(uint8_t reg)
{
    return g_tmp112->regs[reg & 0x3];
}
void fake_tmp112_set_reg(uint8_t reg, uint16_t v)
{
    g_tmp112->regs[reg & 0x3] = v;
}
void fake_tmp112_reset(void)
{
    memset(g_tmp112->regs, 0, sizeof(g_tmp112->regs));
    g_tmp112->regs[1] = 0x60A0; /* CONF power-on default */
    g_tmp112->ptr     = 0;
}

static int fake_tmp112_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                int addr)
{
    ARG_UNUSED(addr);
    struct fake_tmp112_data *d = target->data;
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if ((m->flags & I2C_MSG_READ) != 0u) {
            uint16_t v = d->regs[d->ptr & 0x3];
            if (m->len >= 1) m->buf[0] = (uint8_t)(v >> 8);
            if (m->len >= 2) m->buf[1] = (uint8_t)(v & 0xFF);
        } else {
            if (m->len >= 1) d->ptr = m->buf[0];
            if (m->len >= 3) {
                d->regs[d->ptr & 0x3] = (uint16_t)(((uint16_t)m->buf[1] << 8) | m->buf[2]);
            }
        }
    }
    return 0;
}

static const struct i2c_emul_api fake_tmp112_api = {
    .transfer = fake_tmp112_transfer,
};

static int fake_tmp112_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    g_tmp112 = target->data;
    fake_tmp112_reset();
    return 0;
}

#define FAKE_TMP112_DEFINE(n)                                                                      \
    static struct fake_tmp112_data fake_tmp112_data_##n;                                           \
    EMUL_DT_INST_DEFINE(n, fake_tmp112_init, &fake_tmp112_data_##n, NULL, &fake_tmp112_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_TMP112_DEFINE)
FAKE_EMUL_DEV_SHIM()
