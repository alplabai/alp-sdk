/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake ACT88760 PMIC.  The real chip is TWO I2C slaves: ADD1 (0x25
 * on CMI 120.E1) carries MSTR + GPIO + Buck1..6 tiles; ADD2 (0x26)
 * carries Buck7 + LDO1..6.  One DT node per slave, same compatible;
 * instances self-sort by their DT reg address at init.
 * Register truth: AA82BZ_RegisterMap_Users_Rev1P1 workbook +
 * ACT88760 Datasheet Rev C (verified 2026-06-06).
 */
#define DT_DRV_COMPAT alp_fake_act8760

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_act8760_cfg {
    uint8_t addr_7bit;
};

struct fake_act8760_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_act8760_add1; /* 0x25 */
static struct fake_reg8 *g_act8760_add2; /* 0x26 */

static struct fake_reg8 *slave_for(uint8_t addr_7bit)
{
    return (addr_7bit == 0x26u) ? g_act8760_add2 : g_act8760_add1;
}

uint8_t fake_act8760_get_reg(uint8_t addr_7bit, uint8_t reg)
{
    return slave_for(addr_7bit)->regs[reg];
}
void fake_act8760_set_reg(uint8_t addr_7bit, uint8_t reg, uint8_t val)
{
    slave_for(addr_7bit)->regs[reg] = val;
}
void fake_act8760_reset(void)
{
    fake_reg8_reset(g_act8760_add1);
    fake_reg8_reset(g_act8760_add2);
}

static int fake_act8760_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                 int addr)
{
    ARG_UNUSED(addr);
    struct fake_act8760_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_act8760_api = {
    .transfer = fake_act8760_transfer,
};

static int fake_act8760_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_act8760_data      *data = target->data;
    const struct fake_act8760_cfg *cfg  = target->cfg;
    fake_reg8_reset(&data->rf);
    if (cfg->addr_7bit == 0x26u) {
        g_act8760_add2 = &data->rf;
    } else {
        g_act8760_add1 = &data->rf;
    }
    return 0;
}

#define FAKE_ACT8760_DEFINE(n)                                                                     \
    static struct fake_act8760_data      fake_act8760_data_##n;                                    \
    static const struct fake_act8760_cfg fake_act8760_cfg_##n = {                                  \
        .addr_7bit = DT_INST_REG_ADDR(n),                                                          \
    };                                                                                             \
    EMUL_DT_INST_DEFINE(n, fake_act8760_init, &fake_act8760_data_##n, &fake_act8760_cfg_##n,       \
                        &fake_act8760_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_ACT8760_DEFINE)
FAKE_EMUL_DEV_SHIM()
