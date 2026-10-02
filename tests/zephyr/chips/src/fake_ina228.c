/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI INA228 i2c-emul target.  Big-endian registers of 16, 24 or 40
 * bits (see chips/ina228/ina228.c's file header): a write is [reg, hi, lo];
 * a read is [reg] then exactly as many bytes as the register is wide, most
 * significant first.  Pre-populated with MANUFACTURER_ID (0x3E) = 0x5449 and
 * DEVICE_ID (0x3F) = 0x2281 so ina228_init()'s identity probe succeeds.
 * `fake_ina228_set_absent(true)` makes every transfer fail the way an
 * unanswered address does, to exercise the "not present" result.
 */

#define DT_DRV_COMPAT alp_fake_ina228

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define FAKE_INA228_LOG_CAP 16

struct fake_ina228_data {
	uint64_t regs[64];
	uint32_t write_count[64];
	uint8_t  log_reg[FAKE_INA228_LOG_CAP];
	uint16_t log_val[FAKE_INA228_LOG_CAP];
	uint32_t log_len;
	bool     absent;
};

static struct fake_ina228_data *g_fake_ina228;

/* Register width in bytes (SLYS021A table 7-3). */
static uint8_t reg_width(uint8_t reg)
{
	switch (reg) {
	case 0x04:
	case 0x05:
	case 0x07:
	case 0x08:
		return 3;
	case 0x09:
	case 0x0A:
		return 5;
	default:
		return 2;
	}
}

static void seed_defaults(struct fake_ina228_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	memset(d->log_reg, 0, sizeof d->log_reg);
	memset(d->log_val, 0, sizeof d->log_val);
	d->log_len    = 0;
	d->absent     = false;
	d->regs[0x01] = 0xFB68u; /* ADC_CONFIG reset value */
	d->regs[0x02] = 0x1000u; /* SHUNT_CAL reset value */
	d->regs[0x0B] = 0x0001u; /* DIAG_ALRT reset value */
	d->regs[0x3E] = 0x5449u;
	d->regs[0x3F] = 0x2281u;
}

static int
fake_ina228_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_ina228_data *d = target->data;

	if (d->absent) return -EIO;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* 16-bit big-endian write: [reg, hi, lo]. */
		if (msgs[0].len != 3) return -EIO;
		const uint8_t  reg = msgs[0].buf[0] & 0x3Fu;
		const uint16_t v   = (uint16_t)(((uint16_t)msgs[0].buf[1] << 8) | msgs[0].buf[2]);
		d->regs[reg]       = v;
		d->write_count[reg]++;
		if (d->log_len < FAKE_INA228_LOG_CAP) {
			d->log_reg[d->log_len] = reg;
			d->log_val[d->log_len] = v;
			d->log_len++;
		}
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len < 1) return -EIO;
		const uint8_t reg = msgs[0].buf[0] & 0x3Fu;
		const uint8_t w   = reg_width(reg);
		if (msgs[1].len != w) return -EIO;
		for (uint8_t i = 0; i < w; i++) {
			msgs[1].buf[i] = (uint8_t)(d->regs[reg] >> (8u * (w - 1u - i)));
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_ina228_api = {
	.transfer = fake_ina228_transfer,
};

static int fake_ina228_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_ina228_data *d = target->data;
	g_fake_ina228              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_INA228_DEFINE(n) \
	static struct fake_ina228_data fake_ina228_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_ina228_init, &fake_ina228_data_##n, NULL, &fake_ina228_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_INA228_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint64_t fake_ina228_get_reg(uint8_t reg)
{
	return g_fake_ina228 ? g_fake_ina228->regs[reg & 0x3Fu] : 0u;
}

void fake_ina228_set_reg(uint8_t reg, uint64_t raw)
{
	if (g_fake_ina228) g_fake_ina228->regs[reg & 0x3Fu] = raw;
}

uint32_t fake_ina228_write_count(uint8_t reg)
{
	return g_fake_ina228 ? g_fake_ina228->write_count[reg & 0x3Fu] : 0u;
}

uint32_t fake_ina228_log_len(void)
{
	return g_fake_ina228 ? g_fake_ina228->log_len : 0u;
}

uint8_t fake_ina228_log_reg(uint32_t idx)
{
	return (g_fake_ina228 && idx < FAKE_INA228_LOG_CAP) ? g_fake_ina228->log_reg[idx] : 0u;
}

uint16_t fake_ina228_log_val(uint32_t idx)
{
	return (g_fake_ina228 && idx < FAKE_INA228_LOG_CAP) ? g_fake_ina228->log_val[idx] : 0u;
}

void fake_ina228_set_absent(bool absent)
{
	if (g_fake_ina228) g_fake_ina228->absent = absent;
}

void fake_ina228_reset(void)
{
	if (g_fake_ina228) seed_defaults(g_fake_ina228);
}
