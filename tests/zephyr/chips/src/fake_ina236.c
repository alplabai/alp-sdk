/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI INA236 i2c-emul target.  16-bit big-endian registers (see
 * chips/ina236/ina236.c's file header: write = [reg, hi, lo], read =
 * [reg] then [hi, lo]).  Pre-populated with MFG_ID (0x3E) = 0x5449
 * ("TI") so ina236_init()'s identity probe succeeds; DEVICE_ID
 * (0x3F) is left 0 since the driver doesn't gate on it.
 */

#define DT_DRV_COMPAT alp_fake_ina236

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_MFG_ID 0x3E

struct fake_ina236_data {
	uint16_t regs[256];
	uint32_t write_count[256];
	uint32_t read_count[256];
};

static struct fake_ina236_data *g_fake_ina236;

static void seed_defaults(struct fake_ina236_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	memset(d->read_count, 0, sizeof d->read_count);
	d->regs[REG_MFG_ID] = 0x5449u;
}

static int
fake_ina236_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_ina236_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* 16-bit big-endian write: [reg, hi, lo]. */
		if (msgs[0].len != 3) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		d->regs[reg]      = (uint16_t)(((uint16_t)msgs[0].buf[1] << 8) | msgs[0].buf[2]);
		d->write_count[reg]++;
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		/* [reg]; then read 2 big-endian bytes. */
		if (msgs[0].len < 1 || msgs[1].len != 2) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		uint16_t      v   = d->regs[reg];
		msgs[1].buf[0]    = (uint8_t)(v >> 8);
		msgs[1].buf[1]    = (uint8_t)(v & 0xFFu);
		d->read_count[reg]++;
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_ina236_api = {
	.transfer = fake_ina236_transfer,
};

static int fake_ina236_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_ina236_data *d = target->data;
	g_fake_ina236              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_INA236_DEFINE(n) \
	static struct fake_ina236_data fake_ina236_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_ina236_init, &fake_ina236_data_##n, NULL, &fake_ina236_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_INA236_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint16_t fake_ina236_get_reg(uint8_t reg)
{
	return g_fake_ina236 ? g_fake_ina236->regs[reg] : 0u;
}

void fake_ina236_set_reg(uint8_t reg, uint16_t val)
{
	if (g_fake_ina236) g_fake_ina236->regs[reg] = val;
}

uint32_t fake_ina236_read_count(uint8_t reg)
{
	return g_fake_ina236 ? g_fake_ina236->read_count[reg] : 0u;
}

void fake_ina236_reset(void)
{
	if (g_fake_ina236) seed_defaults(g_fake_ina236);
}
