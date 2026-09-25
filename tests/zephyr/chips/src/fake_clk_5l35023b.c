/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Renesas/IDT 5L35023B VersaClock i2c-emul target.  A generic
 * byte-addressed register echo (plain write; register-address-then-read),
 * plus a per-transfer log of each READ transaction's length, so the ztest
 * can prove clk_5l35023b_register_dump() issues one single-byte read
 * transaction per register rather than a single combined multi-byte burst
 * -- the whole reason that function exists instead of a plain multi-byte
 * i2c_burst_read() (see the driver's file-level comment on the RZ/V2N
 * i2c-riic multi-byte-read corruption it works around).
 *
 * GENERAL_CTRL (0x00) POR value encodes strap_field = 2 (bits[6:5]),
 * matching the fixed I2C address 0x6A this fake answers on -- the driver's
 * own init() ACK-probe compares this field against the address the caller
 * asked for.
 */

#define DT_DRV_COMPAT alp_fake_clk_5l35023b

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_GENERAL_CTRL 0x00u

#define XFER_LOG_CAP 16

struct fake_clk_5l35023b_data {
	uint8_t regs[256];
	uint8_t read_len_log[XFER_LOG_CAP];
	size_t  read_log_len;
};

static struct fake_clk_5l35023b_data *g_fake;

static void seed_defaults(struct fake_clk_5l35023b_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	d->regs[REG_GENERAL_CTRL] = (uint8_t)(2u << 5); /* strap_field 2 -> addr 0x6A */
	d->read_log_len           = 0;
}

static int
fake_clk_5l35023b_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_clk_5l35023b_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* Plain write: [reg, val]. */
		if (msgs[0].len != 2) return -EIO;
		d->regs[msgs[0].buf[0]] = msgs[0].buf[1];
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		/* Register read: [reg]; then read len bytes -- logged so the
		 * ztest can assert the exact shape of every transaction. */
		if (msgs[0].len != 1) return -EIO;
		if (d->read_log_len < XFER_LOG_CAP) {
			d->read_len_log[d->read_log_len++] = (uint8_t)msgs[1].len;
		}
		const uint8_t reg0 = msgs[0].buf[0];
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			msgs[1].buf[i] = d->regs[(uint8_t)(reg0 + i)];
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_clk_5l35023b_api = {
	.transfer = fake_clk_5l35023b_transfer,
};

static int fake_clk_5l35023b_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_clk_5l35023b_data *d = target->data;
	g_fake                           = d;
	seed_defaults(d);
	return 0;
}

/* Bare placeholder device -- see fake_rv3028c7.c's comment above its
 * FAKE_RV3028C7_DEFINE for why this is needed (alp-sdk chip drivers are
 * hand-rolled, not Zephyr `device` drivers, so there is no companion
 * DEVICE_DT_*_DEFINE() elsewhere for this node). */
#define FAKE_CLK_5L35023B_DEFINE(n) \
	static struct fake_clk_5l35023b_data fake_clk_5l35023b_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, \
	                    fake_clk_5l35023b_init, \
	                    &fake_clk_5l35023b_data_##n, \
	                    NULL, \
	                    &fake_clk_5l35023b_api, \
	                    NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_CLK_5L35023B_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

void fake_clk_5l35023b_set_reg(uint8_t reg, uint8_t val)
{
	if (g_fake) g_fake->regs[reg] = val;
}

size_t fake_clk_5l35023b_read_log_len(void)
{
	return g_fake ? g_fake->read_log_len : 0u;
}

uint8_t fake_clk_5l35023b_read_log_at(size_t i)
{
	return g_fake ? g_fake->read_len_log[i] : 0u;
}

void fake_clk_5l35023b_reset(void)
{
	if (g_fake) seed_defaults(g_fake);
}
