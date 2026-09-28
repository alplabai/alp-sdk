/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TCAL9538 / TCA6408A i2c-emul target.  Two instances are wired
 * in the overlay: one at the TCAL9538 strap (0x73, Agile IO block
 * present) and one at the TCA6408A alt-strap (0x20, no Agile IO
 * block) -- the same pair of addresses tcal9538_init() branches its
 * `has_latched_irq` capability flag on.  A plain byte-register echo
 * (same shape as fake_lsm6dso.c) plus a per-register write/read
 * transaction counter, so the chips ztest can assert both exact
 * register values AND that a rejected (NOSUPPORT) call never touches
 * the bus at all.
 */

#define DT_DRV_COMPAT alp_fake_tcal9538

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

struct fake_tcal9538_data {
	uint8_t  addr;
	uint8_t  regs[256];
	uint32_t write_count[256];
	uint32_t read_count[256];
	uint32_t total_transactions;
};

/* Two fixed-address instances (0x73, 0x20); looked up by addr from
 * the test-side inspection API below. */
#define FAKE_TCAL9538_MAX_SLOTS 4
static struct fake_tcal9538_data *g_slots[FAKE_TCAL9538_MAX_SLOTS];
static size_t                     g_slot_count;

static struct fake_tcal9538_data *slot_find(uint8_t addr)
{
	for (size_t i = 0; i < g_slot_count; i++) {
		if (g_slots[i]->addr == addr) return g_slots[i];
	}
	return NULL;
}

static void slot_reset_state(struct fake_tcal9538_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	memset(d->read_count, 0, sizeof d->read_count);
	d->total_transactions = 0;

	/* POR defaults per SCPS280B (tcal9538.h's register-map doc
	 * comment): CFG all-inputs (0xFF), PULL_SEL 0xFF, IRQ_MASK 0xFF --
	 * everything else (OUTPUT, POL, INPUT_LATCH, PULL_EN, IRQ_STATUS)
	 * defaults to 0.  Seeding these matters for
	 * test_tcal9538_set_pull_*: the datasheet's PULL_SEL default of
	 * 0xFF is why set_pull(pin, UP) writes 0x44 back UNCHANGED and
	 * set_pull(pin, DOWN) clears exactly one bit. */
	d->regs[0x03u] = 0xFFu;
	d->regs[0x44u] = 0xFFu;
	d->regs[0x45u] = 0xFFu;
}

static int
fake_tcal9538_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_tcal9538_data *d = target->data;
	d->total_transactions++;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* Plain write: [reg, val0, val1, ...]. */
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
		/* Register read: [reg]; then read len bytes. */
		if (msgs[0].len < 1) return -EIO;
		const uint8_t reg0 = msgs[0].buf[0];
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			uint8_t r      = (uint8_t)(reg0 + i);
			msgs[1].buf[i] = d->regs[r];
			d->read_count[r]++;
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_tcal9538_api = {
	.transfer = fake_tcal9538_transfer,
};

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed -- one per instance. */
#define FAKE_TCAL9538_DEFINE(n) \
	static struct fake_tcal9538_data fake_tcal9538_data_##n = { \
		.addr = (uint8_t)DT_INST_REG_ADDR(n), \
	}; \
	static int fake_tcal9538_init_##n(const struct emul *target, const struct device *parent) \
	{ \
		(void)parent; \
		struct fake_tcal9538_data *d = target->data; \
		slot_reset_state(d); \
		if (g_slot_count < FAKE_TCAL9538_MAX_SLOTS) g_slots[g_slot_count++] = d; \
		return 0; \
	} \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_tcal9538_init_##n, &fake_tcal9538_data_##n, NULL, &fake_tcal9538_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_TCAL9538_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_tcal9538_get_reg(uint8_t addr, uint8_t reg)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	return d ? d->regs[reg] : 0u;
}

void fake_tcal9538_set_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	if (d) d->regs[reg] = val;
}

uint32_t fake_tcal9538_write_count(uint8_t addr, uint8_t reg)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	return d ? d->write_count[reg] : 0u;
}

uint32_t fake_tcal9538_read_count(uint8_t addr, uint8_t reg)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	return d ? d->read_count[reg] : 0u;
}

uint32_t fake_tcal9538_total_transactions(uint8_t addr)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	return d ? d->total_transactions : 0u;
}

void fake_tcal9538_reset(uint8_t addr)
{
	struct fake_tcal9538_data *d = slot_find(addr);
	if (d) slot_reset_state(d);
}
