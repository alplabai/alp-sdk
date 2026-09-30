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
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_VOUT1   0x01u
#define REG_VOUT2   0x02u
#define REG_CONTROL 0x03u
#define REG_STATUS  0x05u

/* CONTROL bit7 -- TPS628640_CTRL_RESET in the real driver's header.  A
 * write with this bit set is a one-shot device reset: every register
 * reverts to its POR/OTP default and the bit self-clears, so the fake
 * must not just store the written byte the way every other register
 * write does. */
#define REG_CONTROL_RESET_BIT 0x80u

struct fake_tps628640_data {
	uint8_t  addr;
	uint8_t  regs[256];
	uint32_t write_count[256];
	bool     fail_next_read_armed;
	uint8_t  fail_next_read_reg;
	bool     fail_next_write_armed;
	uint8_t  fail_next_write_reg;
	uint8_t  fail_next_write_val;
	bool     vout2_por_set;  /* test hook, see fake_tps628640_set_vout2_por() */
	uint8_t  vout2_por_code; /* valid only when vout2_por_set */
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

/* POR/OTP values a real device reset (or a fresh power-up) puts VOUT1 and
 * CONTROL back to.  Split out of slot_reset_state() so a live RESET-bit
 * write can revert just the registers, leaving the write-count log intact
 * for the ztest to inspect. */
static void regs_revert_to_por(struct fake_tps628640_data *d)
{
	const uint8_t por_vout = d->addr == 0x44u ? 0x82u : d->addr == 0x48u ? 0x5Au : 0x14u;
	d->regs[REG_VOUT1]     = por_vout;
	/* VOUT2 has no bench reading of its own (only VOUT1 was probed) --
	 * default to mirroring VOUT1's per-address POR value rather than the
	 * datasheet's generic 0x64, since the OTP that sets VOUT1's startup
	 * target per instance plausibly sets VOUT2 the same way.  TBD-verify
	 * on real silicon; tps628640_software_enable() checks both registers
	 * because the driver cannot read the VID strap that picks which one
	 * is live.  fake_tps628640_set_vout2_por(addr, code) models real
	 * silicon more literally: VOUT2 has its OWN independent OTP-set POR
	 * code, which just happens to equal VOUT1's on every instance this
	 * fake's default models -- the setter overrides that code for one
	 * instance, so a ztest can prove the VOUT2 check is real by giving it
	 * a genuinely different post-reset value from VOUT1, something the
	 * mirrored default can never produce on its own. */
	d->regs[REG_VOUT2]   = d->vout2_por_set ? d->vout2_por_code : por_vout;
	d->regs[REG_CONTROL] = 0x6Fu;
}

static void slot_reset_state(struct fake_tps628640_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->write_count, 0, sizeof d->write_count);
	d->fail_next_read_armed  = false;
	d->fail_next_write_armed = false;
	d->vout2_por_set         = false;
	regs_revert_to_por(d);
}

static int
fake_tps628640_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_tps628640_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len != 2) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		const uint8_t val = msgs[0].buf[1];
		/* Checked (and consumed) BEFORE the write_count bump / any state
		 * change: a NACK'd transaction never lands and never counts as a
		 * write the ztest could mistake for a successful one. Matched on
		 * (reg, val), not just reg, so a specific byte (e.g. the FPWM/
		 * ramp-restore write) can be failed without also failing the
		 * RESET-bit write that always lands on REG_CONTROL first. */
		if (d->fail_next_write_armed && reg == d->fail_next_write_reg &&
		    val == d->fail_next_write_val) {
			d->fail_next_write_armed = false;
			return -EIO;
		}
		d->write_count[reg]++;
		if (reg == REG_CONTROL && (val & REG_CONTROL_RESET_BIT) != 0u) {
			regs_revert_to_por(d);
			return 0;
		}
		d->regs[reg] = reg == REG_CONTROL ? (uint8_t)(val & 0x7Fu) : val;
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len < 1 || msgs[1].len != 1) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		if (d->fail_next_read_armed && reg == d->fail_next_read_reg) {
			d->fail_next_read_armed = false;
			return -EIO;
		}
		msgs[1].buf[0] = d->regs[reg];
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

void fake_tps628640_fail_next_read(uint8_t addr, uint8_t reg)
{
	struct fake_tps628640_data *d = slot_find(addr);
	if (d == NULL) return;
	d->fail_next_read_armed = true;
	d->fail_next_read_reg   = reg;
}

void fake_tps628640_fail_next_write(uint8_t addr, uint8_t reg, uint8_t val)
{
	struct fake_tps628640_data *d = slot_find(addr);
	if (d == NULL) return;
	d->fail_next_write_armed = true;
	d->fail_next_write_reg   = reg;
	d->fail_next_write_val   = val;
}

void fake_tps628640_set_vout2_por(uint8_t addr, uint8_t code)
{
	struct fake_tps628640_data *d = slot_find(addr);
	if (d == NULL) return;
	d->vout2_por_set  = true;
	d->vout2_por_code = code;
}
