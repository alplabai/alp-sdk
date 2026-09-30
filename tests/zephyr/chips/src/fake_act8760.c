/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Qorvo ACT88760 i2c-emul target.  The chip answers on two slave
 * addresses (ADD1 0x25 = MSTR + GPIO + Buck1..6, ADD2 0x26 = Buck7 +
 * LDO1..6); the overlay wires one emul node per address and both share
 * ONE two-page register image here, indexed [page][reg].  Plain byte
 * echo plus the two clear-on-read GPIO toggle registers (ADD1 0x04, and
 * the GPIO9..11 toggle bits 6/3/0 of ADD1 0x2B), a per-register write
 * counter and an ordered write log -- enough for the ztest to assert
 * that a guarded call wrote exactly one byte, or none at all.
 */

#define DT_DRV_COMPAT alp_fake_act8760

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define ADD1_ADDR 0x25u

/* One instance's only state: which page of the shared image it is. */
struct fake_act8760_inst {
	uint8_t page;
};

static uint8_t                   g_regs[2][256];
static uint32_t                  g_write_count[2][256];
static struct fake_act8760_write g_log[FAKE_ACT8760_LOG_MAX];
static size_t                    g_log_len;

static void record_write(uint8_t page, uint8_t reg, uint8_t val)
{
	g_regs[page][reg] = val;
	g_write_count[page][reg]++;
	if (g_log_len < FAKE_ACT8760_LOG_MAX) {
		g_log[g_log_len++] = (struct fake_act8760_write){ page, reg, val };
	}
}

static uint8_t read_reg(uint8_t page, uint8_t reg)
{
	uint8_t v = g_regs[page][reg];
	if (page == 0u && reg == 0x04u) g_regs[0][0x04] = 0u;               /* GPIO1..8 toggles */
	if (page == 0u && reg == 0x2Bu) g_regs[0][0x2B] &= (uint8_t)~0x49u; /* GPIO9..11 */
	return v;
}

static int
fake_act8760_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	const uint8_t page = ((const struct fake_act8760_inst *)target->data)->page;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len < 2) return -EIO;
		for (uint32_t i = 1; i < msgs[0].len; i++) {
			record_write(page, (uint8_t)(msgs[0].buf[0] + i - 1u), msgs[0].buf[i]);
		}
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len < 1) return -EIO;
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			msgs[1].buf[i] = read_reg(page, (uint8_t)(msgs[0].buf[0] + i));
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_act8760_api = {
	.transfer = fake_act8760_transfer,
};

static int fake_act8760_init(const struct emul *target, const struct device *parent)
{
	(void)target;
	(void)parent;
	fake_act8760_reset();
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed -- one per instance. */
#define FAKE_ACT8760_DEFINE(n) \
	static struct fake_act8760_inst fake_act8760_inst_##n = { \
		.page = DT_INST_REG_ADDR(n) == ADD1_ADDR ? 0u : 1u, \
	}; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_act8760_init, &fake_act8760_inst_##n, NULL, &fake_act8760_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_ACT8760_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_act8760_get_reg(uint8_t page, uint8_t reg)
{
	return g_regs[page & 1u][reg];
}

void fake_act8760_set_reg(uint8_t page, uint8_t reg, uint8_t val)
{
	g_regs[page & 1u][reg] = val;
}

uint32_t fake_act8760_write_count(uint8_t page, uint8_t reg)
{
	return g_write_count[page & 1u][reg];
}

size_t fake_act8760_log_len(void)
{
	return g_log_len;
}

const struct fake_act8760_write *fake_act8760_log(size_t i)
{
	return i < g_log_len ? &g_log[i] : NULL;
}

void fake_act8760_reset(void)
{
	memset(g_regs, 0, sizeof g_regs);
	memset(g_write_count, 0, sizeof g_write_count);
	g_log_len = 0;
}
