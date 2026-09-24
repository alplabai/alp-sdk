/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI TPS628640 i2c-emul target.  One instance per V2N-M1 buck
 * address the overlay wires (0x44, 0x48, 0x4F -- 0x4D is taken by
 * fake_tas2563).  POR image = the bench reads: VOUT1 0x82 / 0x5A / 0x14
 * by address, CONTROL 0x6F.  STATUS (0x05) clears on read; the CONTROL
 * RESET bit (7) self-clears.  Per-register write counter per instance,
 * so the ztest can assert a refused call never reached the bus.
 */

#define DT_DRV_COMPAT alp_fake_tps628640

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_VOUT1   0x01u
#define REG_CONTROL 0x03u
#define REG_STATUS  0x05u

struct fake_tps628640_data {
	uint8_t  addr;
	uint8_t  regs[256];
	uint32_t write_count[256];
};

#define FAKE_TPS628640_MAX_SLOTS 4
static struct fake_tps628640_data *g_slots[FAKE_TPS628640_MAX_SLOTS];
static size_t                      g_slot_count;

static struct fake_tps628640_data *slot_find(uint8_t addr)
{
	for (size_t i = 0; i < g_slot_count; i++) {
		if (g_slots[i]->addr == addr) return g_slots[i];
	}
	return NULL;
}

static void slot_reset_state(struct fake_tps628640_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	d->regs[REG_VOUT1]   = d->addr == 0x44u ? 0x82u : d->addr == 0x48u ? 0x5Au : 0x14u;
	d->regs[REG_CONTROL] = 0x6Fu;
}

static int
fake_tps628640_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_tps628640_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len != 2) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		d->write_count[reg]++;
		d->regs[reg] = reg == REG_CONTROL ? (uint8_t)(msgs[0].buf[1] & 0x7Fu) : msgs[0].buf[1];
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len < 1 || msgs[1].len != 1) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		msgs[1].buf[0]    = d->regs[reg];
		if (reg == REG_STATUS) d->regs[reg] = 0u;
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_tps628640_api = {
	.transfer = fake_tps628640_transfer,
};

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed -- one per instance. */
#define FAKE_TPS628640_DEFINE(n) \
	static struct fake_tps628640_data fake_tps628640_data_##n = { \
		.addr = (uint8_t)DT_INST_REG_ADDR(n), \
	}; \
	static int fake_tps628640_init_##n(const struct emul *target, const struct device *parent) \
	{ \
		(void)parent; \
		struct fake_tps628640_data *d = target->data; \
		slot_reset_state(d); \
		if (g_slot_count < FAKE_TPS628640_MAX_SLOTS) g_slots[g_slot_count++] = d; \
		return 0; \
	} \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_tps628640_init_##n, &fake_tps628640_data_##n, NULL, &fake_tps628640_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_TPS628640_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_tps628640_get_reg(uint8_t addr, uint8_t reg)
{
	struct fake_tps628640_data *d = slot_find(addr);
	return d ? d->regs[reg] : 0u;
}

void fake_tps628640_set_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
	struct fake_tps628640_data *d = slot_find(addr);
	if (d) d->regs[reg] = val;
}

uint32_t fake_tps628640_write_count(uint8_t addr, uint8_t reg)
{
	struct fake_tps628640_data *d = slot_find(addr);
	return d ? d->write_count[reg] : 0u;
}

void fake_tps628640_reset(uint8_t addr)
{
	struct fake_tps628640_data *d = slot_find(addr);
	if (d) slot_reset_state(d);
}
