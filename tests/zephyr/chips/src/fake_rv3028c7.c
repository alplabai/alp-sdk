/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Micro Crystal RV-3028-C7 i2c-emul target.
 *
 * Models two distinct memories, per the driver's own EEADDR/EEDATA/
 * EECMD protocol (chips/rv3028c7/rv3028c7.c):
 *   - `regs[256]`   the live RAM registers (STATUS, CONTROL_1/2,
 *                    EEADDR 0x25, EEDATA 0x26, EECMD 0x27, the RAM
 *                    mirror of EEPROM_CLKOUT 0x35 / EEPROM_BACKUP
 *                    0x37, ...) -- a plain write/read echo.
 *   - `eeprom[256]` the EEPROM backing store, indexed by whatever
 *                    byte was last written to EEADDR (regs[0x25]).
 *                    Writing EECMD (0x27) = 0x21 commits
 *                    eeprom[EEADDR] <- regs[EEDATA]; writing
 *                    EECMD = 0x22 loads regs[EEDATA] <- eeprom[EEADDR].
 *                    This is deliberately a SEPARATE array from
 *                    `regs` -- the whole point of the driver's
 *                    endurance guard (rv3028c7_route_clkout /
 *                    rv3028c7_set_int_enable) is to compare the RAM
 *                    mirror against this backing store via a real
 *                    EECMD 0x22 readback, not against itself.
 *
 * Every applied register write (reg, val) is also appended to an
 * ordered log (`wlog`) so the ztest can assert the EXACT, ORDERED
 * multi-transaction EEPROM-commit sequence
 * (0x25=addr, 0x26=data, 0x27=0x00, 0x27=0x21) the driver issues --
 * something a last-value-only register echo can't distinguish from
 * a differently-ordered or partially-skipped sequence that happens
 * to land on the same final register contents.
 *
 * `fake_rv3028c7_fail_next_write()` arms a one-shot NACK for a
 * SPECIFIC (reg, val) pair -- not just "the next write" -- so a test
 * can fail exactly one step of a multi-write commit (e.g. the final
 * EECMD=0x21) without disturbing the steps before it, to prove a
 * retry re-attempts the commit instead of the guard wrongly treating
 * the failed attempt as already applied.
 */

#define DT_DRV_COMPAT alp_fake_rv3028c7

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_EE_ADDR  0x25u
#define REG_EE_DATA  0x26u
#define REG_EE_CMD   0x27u
#define EE_CMD_WRITE 0x21u
#define EE_CMD_READ  0x22u

#define WLOG_CAP 128

struct fake_rv3028c7_data {
	uint8_t regs[256];
	uint8_t eeprom[256];

	struct {
		uint8_t reg;
		uint8_t val;
	} wlog[WLOG_CAP];
	size_t wlog_len;

	bool    fail_armed;
	uint8_t fail_reg;
	uint8_t fail_val;
};

static struct fake_rv3028c7_data *g_fake_rv3028c7;

static void seed_defaults(struct fake_rv3028c7_data *d)
{
	memset(d->regs, 0, sizeof d->regs);
	memset(d->eeprom, 0, sizeof d->eeprom);
	d->wlog_len   = 0;
	d->fail_armed = false;
}

static void apply_write(struct fake_rv3028c7_data *d, uint8_t reg, uint8_t val)
{
	if (d->wlog_len < WLOG_CAP) {
		d->wlog[d->wlog_len].reg = reg;
		d->wlog[d->wlog_len].val = val;
		d->wlog_len++;
	}
	d->regs[reg] = val;

	if (reg == REG_EE_CMD) {
		if (val == EE_CMD_WRITE) {
			d->eeprom[d->regs[REG_EE_ADDR]] = d->regs[REG_EE_DATA];
		} else if (val == EE_CMD_READ) {
			d->regs[REG_EE_DATA] = d->eeprom[d->regs[REG_EE_ADDR]];
		}
	}
}

static int
fake_rv3028c7_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_rv3028c7_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		/* Plain write: [reg, val0, val1, ...]. */
		if (msgs[0].len < 1) return -EIO;
		const uint8_t reg0 = msgs[0].buf[0];
		for (uint32_t i = 1; i < msgs[0].len; i++) {
			uint8_t r = (uint8_t)(reg0 + i - 1);
			uint8_t v = msgs[0].buf[i];
			if (d->fail_armed && r == d->fail_reg && v == d->fail_val) {
				d->fail_armed = false;
				return -EIO; /* NACK: not applied, not logged. */
			}
			apply_write(d, r, v);
		}
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		/* Register read: [reg]; then read len bytes. */
		if (msgs[0].len < 1) return -EIO;
		const uint8_t reg0 = msgs[0].buf[0];
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			msgs[1].buf[i] = d->regs[(uint8_t)(reg0 + i)];
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_rv3028c7_api = {
	.transfer = fake_rv3028c7_transfer,
};

static int fake_rv3028c7_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_rv3028c7_data *d = target->data;
	g_fake_rv3028c7              = d;
	seed_defaults(d);
	return 0;
}

/* alp-sdk chip drivers are hand-rolled (not Zephyr `device` drivers),
 * so there is no companion DEVICE_DT_*_DEFINE() elsewhere for this
 * node the way a real Zephyr sensor driver would provide one --
 * EMUL_DT_DEFINE()'s `.dev = DEVICE_DT_GET(node_id)` requires ONE to
 * exist (see the "MUST have a corresponding DEVICE_DT_DEFINE()" note
 * on EMUL_DT_DEFINE, <zephyr/drivers/emul.h>), so this defines a
 * bare, API-less placeholder device solely to satisfy that link-time
 * requirement -- nothing in this test ever calls into it. */
#define FAKE_RV3028C7_DEFINE(n) \
	static struct fake_rv3028c7_data fake_rv3028c7_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_rv3028c7_init, &fake_rv3028c7_data_##n, NULL, &fake_rv3028c7_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_RV3028C7_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_rv3028c7_get_reg(uint8_t reg)
{
	return g_fake_rv3028c7 ? g_fake_rv3028c7->regs[reg] : 0u;
}

void fake_rv3028c7_set_reg(uint8_t reg, uint8_t val)
{
	if (g_fake_rv3028c7) g_fake_rv3028c7->regs[reg] = val;
}

uint8_t fake_rv3028c7_get_eeprom(uint8_t addr)
{
	return g_fake_rv3028c7 ? g_fake_rv3028c7->eeprom[addr] : 0u;
}

void fake_rv3028c7_set_eeprom(uint8_t addr, uint8_t val)
{
	if (g_fake_rv3028c7) g_fake_rv3028c7->eeprom[addr] = val;
}

size_t fake_rv3028c7_wlog_len(void)
{
	return g_fake_rv3028c7 ? g_fake_rv3028c7->wlog_len : 0u;
}

uint8_t fake_rv3028c7_wlog_reg(size_t i)
{
	return g_fake_rv3028c7 ? g_fake_rv3028c7->wlog[i].reg : 0u;
}

uint8_t fake_rv3028c7_wlog_val(size_t i)
{
	return g_fake_rv3028c7 ? g_fake_rv3028c7->wlog[i].val : 0u;
}

void fake_rv3028c7_wlog_reset(void)
{
	if (g_fake_rv3028c7) g_fake_rv3028c7->wlog_len = 0;
}

void fake_rv3028c7_fail_next_write(uint8_t reg, uint8_t val)
{
	if (!g_fake_rv3028c7) return;
	g_fake_rv3028c7->fail_armed = true;
	g_fake_rv3028c7->fail_reg   = reg;
	g_fake_rv3028c7->fail_val   = val;
}

void fake_rv3028c7_reset(void)
{
	if (g_fake_rv3028c7) seed_defaults(g_fake_rv3028c7);
}
