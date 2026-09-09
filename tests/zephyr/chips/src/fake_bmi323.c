/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Bosch BMI323 i2c-emul target.  Unlike the other fakes in this
 * directory, the BMI323 wire protocol is 16-bit-per-register (LSB
 * first) with a 2-byte dummy prefix on every read (see chips/bmi323/
 * bmi323.c's file header) -- so the register file here is
 * `uint16_t[256]`, not a raw byte array, and the transfer function
 * models exactly those two shapes: a 3-byte write `[reg, lo, hi]`,
 * and a `[reg]`-then-`(2 + words*2)`-byte read.  Pre-populated with
 * CHIP_ID (register 0x00, low byte) = 0x43 and STATUS (register
 * 0x02) bit0 (por_detected) = 1 -- the "just reset" state -- so
 * bmi323_init()'s soft-reset + device-initialisation status test +
 * ID probe (chips/bmi323/bmi323.c) succeeds by default.
 *
 * STATUS also models the datasheet's read/clear behaviour
 * (BST-BMI323-DS000-13 Rev 1.7 p.66: por_detected bit0 / drdy_gyr
 * bit6 / drdy_acc bit7 all clear on read) -- any read that covers
 * register 0x02 zeroes those three bits afterwards.  #2035 shipped
 * because this wasn't modelled: bmi323_init() consumed por_detected
 * and every later reader silently saw 0, and no fake in this suite
 * caught it because none of them cleared anything on read.
 */

#define DT_DRV_COMPAT alp_fake_bmi323

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_CHIP_ID 0x00
#define REG_STATUS  0x02
/* por_detected (bit0) / drdy_gyr (bit6) / drdy_acc (bit7), the three
 * clear-on-read STATUS bits (BST-BMI323-DS000-13 Rev 1.7 p.66). */
#define STATUS_CLEAR_ON_READ_MASK 0x00C1u

struct fake_bmi323_data {
	uint16_t regs[256];
	uint32_t write_count[256];
};

static struct fake_bmi323_data *g_fake_bmi323;

static void seed_defaults(struct fake_bmi323_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	d->regs[REG_CHIP_ID] = 0x0043u; /* BMI323_CHIP_ID in the low byte. */
	d->regs[REG_STATUS]  = 0x0001u; /* por_detected -- a fresh reset already happened. */
}

static int
fake_bmi323_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_bmi323_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* 16-bit write: [reg, lo, hi]. */
		if (msgs[0].len != 3) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		d->regs[reg]      = (uint16_t)(msgs[0].buf[1] | ((uint16_t)msgs[0].buf[2] << 8));
		d->write_count[reg]++;
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		/* [reg]; then read 2 dummy bytes + words*2 data bytes. */
		if (msgs[0].len < 1) return -EIO;
		if (msgs[1].len < 2 || (msgs[1].len % 2) != 0) return -EIO;
		const uint8_t reg0  = msgs[0].buf[0];
		const size_t  words = (msgs[1].len - 2) / 2;
		msgs[1].buf[0]      = 0x00; /* dummy prefix */
		msgs[1].buf[1]      = 0x00;
		for (size_t w = 0; w < words; w++) {
			const uint8_t reg          = (uint8_t)(reg0 + w);
			uint16_t      v            = d->regs[reg];
			msgs[1].buf[2 + 2 * w]     = (uint8_t)(v & 0xFFu);
			msgs[1].buf[2 + 2 * w + 1] = (uint8_t)(v >> 8);
			/* Clear-on-read: consume por_detected/drdy_gyr/drdy_acc the
			 * same way real silicon does, so a driver bug that reads
			 * STATUS and forgets to keep the bit (#2035) reddens a test
			 * instead of silently passing. */
			if (reg == REG_STATUS) d->regs[reg] = (uint16_t)(v & ~STATUS_CLEAR_ON_READ_MASK);
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_bmi323_api = {
	.transfer = fake_bmi323_transfer,
};

static int fake_bmi323_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_bmi323_data *d = target->data;
	g_fake_bmi323              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_BMI323_DEFINE(n) \
	static struct fake_bmi323_data fake_bmi323_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_bmi323_init, &fake_bmi323_data_##n, NULL, &fake_bmi323_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_BMI323_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint16_t fake_bmi323_get_reg(uint8_t reg)
{
	return g_fake_bmi323 ? g_fake_bmi323->regs[reg] : 0u;
}

void fake_bmi323_set_reg(uint8_t reg, uint16_t val)
{
	if (g_fake_bmi323) g_fake_bmi323->regs[reg] = val;
}

uint32_t fake_bmi323_write_count(uint8_t reg)
{
	return g_fake_bmi323 ? g_fake_bmi323->write_count[reg] : 0u;
}

void fake_bmi323_reset(void)
{
	if (g_fake_bmi323) seed_defaults(g_fake_bmi323);
}
