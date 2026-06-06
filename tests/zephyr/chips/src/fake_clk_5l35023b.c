/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Renesas 5L35023B clock generator.  Seeds the register state
 * matching a V2N-strapped board: Byte 0x00 General Control with
 * I2C_addr strap bits[6:5] = 0 (-> expected slave 0x68), Byte 0x01
 * Dash Code ID = 0x5A (arbitrary factory stamp the test round-trips),
 * and Byte 0x24 bit7 (I2C_PDB) = 1 (normal operation).
 * No write hook needed.
 */
#define DT_DRV_COMPAT alp_fake_clk_5l35023b

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_clk_5l35023b_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_clk_5l35023b;

static void              fake_clk_5l35023b_seed(struct fake_reg8 *f)
{
    /* Byte 0x00 General Control: I2C_addr strap field bits[6:5] = 0
     * -> the part claims slave 0x68.  Byte 0x01 Dash Code ID: an
     * arbitrary factory stamp the test asserts round-trips.  Byte
     * 0x24 bit7 (I2C_PDB) = 1 -> normal operation. */
    f->regs[0x00] = 0x00;
    f->regs[0x01] = 0x5A;
    f->regs[0x24] = 0x80;
}

uint8_t fake_clk_5l35023b_get_reg(uint8_t reg)
{
    return g_clk_5l35023b->regs[reg];
}
void fake_clk_5l35023b_set_reg(uint8_t reg, uint8_t v)
{
    g_clk_5l35023b->regs[reg] = v;
}
void fake_clk_5l35023b_reset(void)
{
    fake_reg8_reset(g_clk_5l35023b);
    fake_clk_5l35023b_seed(g_clk_5l35023b);
}

static int fake_clk_5l35023b_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                      int addr)
{
    ARG_UNUSED(addr);
    struct fake_clk_5l35023b_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_clk_5l35023b_api = {
    .transfer = fake_clk_5l35023b_transfer,
};

static int fake_clk_5l35023b_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_clk_5l35023b_data *data = target->data;
    fake_reg8_reset(&data->rf);
    fake_clk_5l35023b_seed(&data->rf);
    g_clk_5l35023b = &data->rf;
    return 0;
}

#define FAKE_CLK_5L35023B_DEFINE(n)                                                                \
    static struct fake_clk_5l35023b_data fake_clk_5l35023b_data_##n;                               \
    EMUL_DT_INST_DEFINE(n, fake_clk_5l35023b_init, &fake_clk_5l35023b_data_##n, NULL,              \
                        &fake_clk_5l35023b_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_CLK_5L35023B_DEFINE)
FAKE_EMUL_DEV_SHIM()
