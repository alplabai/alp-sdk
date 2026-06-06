/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake DA9292 PMIC.  Register truth: DA9292 Datasheet Rev 2.2
 * (R16DS0518EJ0220).  Seeds the DA9292-AROVx V2N boot state the
 * driver must cope with: CH2_VSTEP=1 in PMC_CTRL_01 (the 1.5 V OTP
 * trap) and the Table-24 description-table VOUT default 0xA3.
 * PMC_EVENT_00/01 are RWC1 (write-1-to-clear) via the write hook.
 */
#define DT_DRV_COMPAT alp_fake_da9292

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_da9292_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_da9292;

static void              da9292_write_hook(struct fake_reg8 *f, uint8_t reg, uint8_t val)
{
    if (reg == 0x02u || reg == 0x03u) { /* PMC_EVENT_00/01: RWC1 */
        f->regs[reg] &= (uint8_t)~val;
    } else {
        f->regs[reg] = val;
    }
}

static void fake_da9292_seed(struct fake_reg8 *f)
{
    f->regs[0x04] = 0xFF; /* PMC_MASK_00 reset (Table 12)         */
    f->regs[0x05] = 0x07; /* PMC_MASK_01 reset                    */
    f->regs[0x06] = 0x04; /* PMC_CTRL_00: 2-ch mode, CONF=GND     */
    f->regs[0x07] = 0x80; /* PMC_CTRL_01: CH2_VSTEP=1 (AROVx OTP) */
    f->regs[0x08] = 0x3F; /* PMC_CTRL_02 reset                    */
    f->regs[0x09] = 0xFF; /* PMC_CTRL_03 reset                    */
    f->regs[0x0A] = 0xA3; /* VOUT_CH1_00 = 0.815 V (Table 24)     */
    f->regs[0x0B] = 0xA3;
    f->regs[0x0C] = 0xA3;
    f->regs[0x0D] = 0xA3;
    f->regs[0x19] = 0xEA; /* PMC_DEV_ID reset                     */
    f->regs[0x1A] = 0x10; /* PMC_REV_ID reset                     */
}

uint8_t fake_da9292_get_reg(uint8_t reg)
{
    return g_da9292->regs[reg];
}
void fake_da9292_set_reg(uint8_t reg, uint8_t v)
{
    g_da9292->regs[reg] = v;
}
uint8_t fake_da9292_log_count(void)
{
    return g_da9292->log_count;
}
void fake_da9292_log_at(uint8_t i, uint8_t *reg, uint8_t *val)
{
    *reg = g_da9292->log[i].reg;
    *val = g_da9292->log[i].val;
}
void fake_da9292_reset(void)
{
    fake_reg8_reset(g_da9292);
    fake_da9292_seed(g_da9292);
}

static int fake_da9292_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                int addr)
{
    ARG_UNUSED(addr);
    struct fake_da9292_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_da9292_api = {
    .transfer = fake_da9292_transfer,
};

static int fake_da9292_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_da9292_data *data = target->data;
    data->rf.write_hook           = da9292_write_hook;
    fake_reg8_reset(&data->rf);
    fake_da9292_seed(&data->rf);
    g_da9292 = &data->rf;
    return 0;
}

#define FAKE_DA9292_DEFINE(n)                                                                      \
    static struct fake_da9292_data fake_da9292_data_##n;                                           \
    EMUL_DT_INST_DEFINE(n, fake_da9292_init, &fake_da9292_data_##n, NULL, &fake_da9292_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_DA9292_DEFINE)
FAKE_EMUL_DEV_SHIM()
