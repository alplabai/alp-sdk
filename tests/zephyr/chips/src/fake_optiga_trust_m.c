/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Infineon OPTIGA Trust M secure element.  The driver probes by
 * reading 4 bytes from the I2C_STATE register (0x82); an idle Trust M
 * reports BUSY=0 / RESP_RDY=0 in the first byte.  All zeros is the
 * canonical idle answer the driver's probe accepts.
 * No write hook needed.
 */
#define DT_DRV_COMPAT alp_fake_optiga_trust_m

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_optiga_trust_m_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_optiga;

static void              fake_optiga_seed(struct fake_reg8 *f)
{
    /* I2C_STATE register (0x82) reads 4 bytes; an idle Trust M
     * reports BUSY=0 / RESP_RDY=0 in the first byte.  All zeros is
     * the canonical idle answer the driver's probe accepts. */
    f->regs[0x82] = 0x00;
    f->regs[0x83] = 0x00;
    f->regs[0x84] = 0x00;
    f->regs[0x85] = 0x00;
}

uint8_t fake_optiga_get_reg(uint8_t reg)
{
    return g_optiga->regs[reg];
}
void fake_optiga_set_reg(uint8_t reg, uint8_t v)
{
    g_optiga->regs[reg] = v;
}
void fake_optiga_reset(void)
{
    fake_reg8_reset(g_optiga);
    fake_optiga_seed(g_optiga);
}

static int fake_optiga_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                int addr)
{
    ARG_UNUSED(addr);
    struct fake_optiga_trust_m_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_optiga_api = {
    .transfer = fake_optiga_transfer,
};

static int fake_optiga_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_optiga_trust_m_data *data = target->data;
    fake_reg8_reset(&data->rf);
    fake_optiga_seed(&data->rf);
    g_optiga = &data->rf;
    return 0;
}

#define FAKE_OPTIGA_DEFINE(n)                                                                      \
    static struct fake_optiga_trust_m_data fake_optiga_trust_m_data_##n;                           \
    EMUL_DT_INST_DEFINE(n, fake_optiga_init, &fake_optiga_trust_m_data_##n, NULL,                  \
                        &fake_optiga_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_OPTIGA_DEFINE)
FAKE_EMUL_DEV_SHIM()
