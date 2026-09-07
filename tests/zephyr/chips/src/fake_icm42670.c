/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TDK ICM-42670-P i2c-emul target.  Byte-register echo (same
 * shape as fake_lsm6dso.c) pre-populated with WHO_AM_I = 0x67 so
 * icm42670_init() succeeds, plus a per-register write counter so the
 * chips ztest can pin exact register writes for the interrupt-config
 * paths (configure_int_pin / route_int / data_ready).
 */

#define DT_DRV_COMPAT alp_fake_icm42670

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_WHO_AM_I 0x75

struct fake_icm42670_data {
	uint8_t  regs[256];
	uint32_t write_count[256];
};

static struct fake_icm42670_data *g_fake_icm42670;

static void seed_defaults(struct fake_icm42670_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	d->regs[REG_WHO_AM_I] = 0x67u;
}

static int
fake_icm42670_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_icm42670_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len < 1) return -EIO;
		const uint8_t reg0 = msgs[0].buf[0];
		for (uint32_t i = 1; i < msgs[0].len; i++) {
			uint8_t r  = (uint8_t)(reg0 + i - 1);
			d->regs[r] = msgs[0].buf[i];
			d->write_count[r]++;
		}
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
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

static const struct i2c_emul_api fake_icm42670_api = {
	.transfer = fake_icm42670_transfer,
};

static int fake_icm42670_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_icm42670_data *d = target->data;
	g_fake_icm42670              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_ICM42670_DEFINE(n) \
	static struct fake_icm42670_data fake_icm42670_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_icm42670_init, &fake_icm42670_data_##n, NULL, &fake_icm42670_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_ICM42670_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_icm42670_get_reg(uint8_t reg)
{
	return g_fake_icm42670 ? g_fake_icm42670->regs[reg] : 0u;
}

void fake_icm42670_set_reg(uint8_t reg, uint8_t val)
{
	if (g_fake_icm42670) g_fake_icm42670->regs[reg] = val;
}

uint32_t fake_icm42670_write_count(uint8_t reg)
{
	return g_fake_icm42670 ? g_fake_icm42670->write_count[reg] : 0u;
}

void fake_icm42670_reset(void)
{
	if (g_fake_icm42670) seed_defaults(g_fake_icm42670);
}
