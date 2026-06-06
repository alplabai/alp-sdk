/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI TPS628640 single-channel buck.  Seeds VOUT1 = 0x28
 * (400 + 40*5 = 600 mV, the LPDDR4X 0.6 V role this instance plays
 * at 0x4D on the V2N) and VOUT2 = 0x28.  The CONTROL register (0x03)
 * is write-only on real silicon; the fake simply stores whatever the
 * driver writes so tests can inspect the shadow RMW behaviour.
 * No write hook needed.
 */
#define DT_DRV_COMPAT alp_fake_tps628640

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "alp/chips/tps628640.h"
#include "fake_reg8.h"
#include "fakes.h"

struct fake_tps628640_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_tps628640;

static void              fake_tps628640_seed(struct fake_reg8 *f)
{
    /* VOUT1 = 0x28 -> 400 + 40*5 = 600 mV (the LPDDR4X 0.6 V role
     * this instance plays at 0x4D on the V2N).  STATUS clear. */
    f->regs[TPS628640_REG_VOUT1] = 0x28;
    f->regs[TPS628640_REG_VOUT2] = 0x28;
}

uint8_t fake_tps628640_get_reg(uint8_t reg)
{
    return g_tps628640->regs[reg];
}
void fake_tps628640_set_reg(uint8_t reg, uint8_t v)
{
    g_tps628640->regs[reg] = v;
}
void fake_tps628640_reset(void)
{
    fake_reg8_reset(g_tps628640);
    fake_tps628640_seed(g_tps628640);
}

static int fake_tps628640_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                   int addr)
{
    ARG_UNUSED(addr);
    struct fake_tps628640_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_tps628640_api = {
    .transfer = fake_tps628640_transfer,
};

static int fake_tps628640_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_tps628640_data *data = target->data;
    fake_reg8_reset(&data->rf);
    fake_tps628640_seed(&data->rf);
    g_tps628640 = &data->rf;
    return 0;
}

#define FAKE_TPS628640_DEFINE(n)                                                                   \
    static struct fake_tps628640_data fake_tps628640_data_##n;                                     \
    EMUL_DT_INST_DEFINE(n, fake_tps628640_init, &fake_tps628640_data_##n, NULL,                    \
                        &fake_tps628640_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_TPS628640_DEFINE)
FAKE_EMUL_DEV_SHIM()
