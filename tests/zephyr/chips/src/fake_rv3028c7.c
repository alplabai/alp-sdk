/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Micro Crystal RV-3028-C7 RTC.  Seeds a plausible factory
 * power-on state: PORF set in STATUS (the driver must clear it on
 * init), CONTROL_2 zero (the driver must set the 24H bit), and a
 * BCD wall time of 2026-06-06 12:34:56, Saturday (weekday 7).
 * No write hook needed -- the driver uses plain read-modify-write.
 */
#define DT_DRV_COMPAT alp_fake_rv3028c7

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_rv3028c7_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_rv3028c7;

static void              fake_rv3028c7_seed(struct fake_reg8 *f)
{
    /* Fresh-from-the-factory state: PORF set (the driver must clear
     * it), CONTROL_2 zero (driver must set the 24H bit), a plausible
     * BCD wall time 2026-06-06 12:34:56, Saturday(7). */
    f->regs[0x00] = 0x56; /* SECONDS, BCD */
    f->regs[0x01] = 0x34; /* MINUTES      */
    f->regs[0x02] = 0x12; /* HOURS        */
    f->regs[0x03] = 0x07; /* WEEKDAY      */
    f->regs[0x04] = 0x06; /* DATE         */
    f->regs[0x05] = 0x06; /* MONTH        */
    f->regs[0x06] = 0x26; /* YEAR (2026)  */
    f->regs[0x0E] = 0x01; /* STATUS: PORF */
}

uint8_t fake_rv3028c7_get_reg(uint8_t reg)
{
    return g_rv3028c7->regs[reg];
}
void fake_rv3028c7_set_reg(uint8_t reg, uint8_t v)
{
    g_rv3028c7->regs[reg] = v;
}
void fake_rv3028c7_reset(void)
{
    fake_reg8_reset(g_rv3028c7);
    fake_rv3028c7_seed(g_rv3028c7);
}

static int fake_rv3028c7_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                  int addr)
{
    ARG_UNUSED(addr);
    struct fake_rv3028c7_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_rv3028c7_api = {
    .transfer = fake_rv3028c7_transfer,
};

static int fake_rv3028c7_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_rv3028c7_data *data = target->data;
    fake_reg8_reset(&data->rf);
    fake_rv3028c7_seed(&data->rf);
    g_rv3028c7 = &data->rf;
    return 0;
}

#define FAKE_RV3028C7_DEFINE(n)                                                                    \
    static struct fake_rv3028c7_data fake_rv3028c7_data_##n;                                       \
    EMUL_DT_INST_DEFINE(n, fake_rv3028c7_init, &fake_rv3028c7_data_##n, NULL, &fake_rv3028c7_api,  \
                        NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_RV3028C7_DEFINE)
FAKE_EMUL_DEV_SHIM()
