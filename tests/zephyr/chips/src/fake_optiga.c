/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake OPTIGA Trust M i2c-emul target.  Answers optiga_trust_m_init()'s
 * probe the way the silicon does: a write of the register address, a
 * STOP, then a separate 4-byte read of I2C_STATE.  A repeated-start
 * write-read is NACKed (bench, E1M-V2M103 2026W38-0001: `i2ctransfer
 * w1@0x30 0x82 r4@0x30` always fails, the split form reads 08 80 00 00).
 * Once armed with
 * fake_optiga_arm_sleep(n), NACKs (-EIO) the next n accesses first --
 * the part NACKs the first access after its idle sleep (bench, E1M-V2M103
 * 2026W38-0001).
 */

#define DT_DRV_COMPAT alp_fake_optiga

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_I2C_STATE 0x82u

struct fake_optiga_data {
	unsigned sleep_nacks;
	uint32_t attempts;
	uint8_t  reg; /* register pointer latched by the last write */
};

static struct fake_optiga_data *g_fake_optiga;

static int
fake_optiga_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_optiga_data *d = target->data;

	d->attempts++;
	if (d->sleep_nacks > 0u) {
		d->sleep_nacks--;
		return -EIO;
	}
	if (num_msgs != 1) return -EIO; /* no repeated start */
	if ((msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len < 1u) return -EIO;
		d->reg = msgs[0].buf[0];
		return 0;
	}
	if (d->reg == REG_I2C_STATE && msgs[0].len == 4u) {
		/* I2C_STATE: nothing pending, max packet size 0x0110 (datasheet default). */
		msgs[0].buf[0] = 0x08u;
		msgs[0].buf[1] = 0x00u;
		msgs[0].buf[2] = 0x01u;
		msgs[0].buf[3] = 0x10u;
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_optiga_api = {
	.transfer = fake_optiga_transfer,
};

static int fake_optiga_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_optiga_data *d = target->data;
	g_fake_optiga              = d;
	d->sleep_nacks             = 0u;
	d->attempts                = 0u;
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_OPTIGA_DEFINE(n) \
	static struct fake_optiga_data fake_optiga_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_optiga_init, &fake_optiga_data_##n, NULL, &fake_optiga_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_OPTIGA_DEFINE)

void fake_optiga_arm_sleep(unsigned count)
{
	if (g_fake_optiga) g_fake_optiga->sleep_nacks = count;
}

uint32_t fake_optiga_attempts(void)
{
	return g_fake_optiga ? g_fake_optiga->attempts : 0u;
}

void fake_optiga_reset(void)
{
	if (g_fake_optiga) {
		g_fake_optiga->sleep_nacks = 0u;
		g_fake_optiga->attempts    = 0u;
	}
}
