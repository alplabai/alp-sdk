/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI TAS2563 i2c-emul target.  Byte-register echo like
 * fake_icm42670.c, plus the three things that make this part's wire
 * protocol different from a flat register file (all per TI SLASET3D,
 * April 2019 rev. January 2024):
 *
 *   1. Book/page paging (§7.3.10 "Register Organization", p.34).  The
 *      register file modelled here is book 0 / page 0 only; a write
 *      aimed anywhere else is recorded in the write log but not
 *      stored, which is all the tuning-replay tests need.  BOOK
 *      (0x7F) is reachable only from page 0 (§7.5.62, p.94), so a
 *      BOOK write from another page is NACKed -- that is how a driver
 *      that forgets to select page 0 first gets caught.
 *   2. Datasheet reset values, so a read-modify-write that clobbers a
 *      neighbouring field shows up as a wrong byte rather than as
 *      zero-vs-zero.
 *   3. The self-clearing CLR_INTP_LTCH bit (0x30 bit 2, §7.5.43
 *      Table 7-143, p.86): setting it wipes INT_LTCH0/1/3/4 and reads
 *      back as 0.
 *
 * The ordered write log lets a test assert the SEQUENCE of writes,
 * not just the final register state -- which is the whole point for
 * the tuning loader (paging order) and for IV-sense enable (power the
 * sense blocks up before handing them a transmit slot).
 */

#define DT_DRV_COMPAT alp_fake_tas2563

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_PAGE       0x00u
#define REG_BOOK       0x7Fu
#define REG_INT_CLK    0x30u
#define INT_CLK_CLR    0x04u
#define REG_LTCH_FIRST 0x24u
#define REG_LTCH_LAST  0x27u

struct fake_tas2563_data {
	uint8_t  regs[256]; /* Book 0 / page 0 only. */
	uint32_t write_count[256];
	uint8_t  cur_book;
	uint8_t  cur_page;

	struct fake_tas2563_write log[FAKE_TAS2563_LOG_MAX];
	size_t                    log_len;

	bool    fault_armed;
	uint8_t fault_book;
	uint8_t fault_page;
	uint8_t fault_reg;
};

static struct fake_tas2563_data *g_fake_tas2563;

/* Reset values, SLASET3D §7.5.x "[reset=..]" headings, p.65-94.  Only
 * the registers this driver reads back matter; the rest stay 0. */
static void seed_defaults(struct fake_tas2563_data *d)
{
	memset(d, 0, sizeof *d);
	d->regs[0x02u] = 0x0Eu; /* PWR_CTL     §7.5.4  p.65 */
	d->regs[0x03u] = 0x20u; /* PB_CFG1     §7.5.5  p.66 */
	d->regs[0x04u] = 0xC6u; /* MISC_CFG1   §7.5.6  p.67 */
	d->regs[0x05u] = 0x22u; /* MISC_CFG2   §7.5.7  p.68 */
	d->regs[0x06u] = 0x09u; /* TDM_CFG0    §7.5.8  p.68 */
	d->regs[0x07u] = 0x02u; /* TDM_CFG1    §7.5.9  p.69 */
	d->regs[0x08u] = 0x4Au; /* TDM_CFG2    §7.5.10 p.69 */
	d->regs[0x09u] = 0x10u; /* TDM_CFG3    §7.5.11 p.70 */
	d->regs[0x0Au] = 0x13u; /* TDM_CFG4    §7.5.12 p.70 */
	d->regs[0x0Bu] = 0x02u; /* TDM_CFG5    §7.5.13 p.71 */
	d->regs[0x0Cu] = 0x00u; /* TDM_CFG6    §7.5.14 p.71 */
	d->regs[0x1Au] = 0xFCu; /* INT_MASK0   §7.5.28 p.77 */
	d->regs[0x1Bu] = 0xA6u; /* INT_MASK1   §7.5.29 p.78 */
	d->regs[0x1Cu] = 0xDFu; /* INT_MASK2   §7.5.30 p.79 */
	d->regs[0x1Du] = 0xFFu; /* INT_MASK3   §7.5.31 p.79 */
	d->regs[0x30u] = 0x19u; /* INT&CLK CFG §7.5.43 p.86 */
}

static void log_write(struct fake_tas2563_data *d, uint8_t reg, uint8_t val)
{
	if (d->log_len >= FAKE_TAS2563_LOG_MAX) return;
	d->log[d->log_len++] = (struct fake_tas2563_write){
		.book = d->cur_book,
		.page = d->cur_page,
		.reg  = reg,
		.val  = val,
	};
}

static int apply_write(struct fake_tas2563_data *d, uint8_t reg, uint8_t val)
{
	if (d->fault_armed && d->fault_book == d->cur_book && d->fault_page == d->cur_page &&
	    d->fault_reg == reg) {
		d->fault_armed = false;
		return -EIO;
	}

	/* BOOK is documented at page 0 only; a driver that switches book
	 * without selecting page 0 first would be addressing something
	 * else entirely. */
	if (reg == REG_BOOK && d->cur_page != 0u) return -EIO;

	log_write(d, reg, val);

	if (reg == REG_PAGE) {
		d->cur_page  = val;
		d->regs[reg] = val;
		d->write_count[reg]++;
		return 0;
	}
	if (reg == REG_BOOK) {
		d->cur_book  = val;
		d->regs[reg] = val;
		d->write_count[reg]++;
		return 0;
	}
	if (d->cur_book != 0u || d->cur_page != 0u) {
		/* Off the modelled page: the write is logged (that is what
		 * the tuning-replay tests check) but there is no backing
		 * store for it. */
		return 0;
	}

	if (reg == REG_INT_CLK && (val & INT_CLK_CLR) != 0u) {
		/* CLR_INTP_LTCH is self clearing and wipes the latches. */
		for (uint8_t r = REG_LTCH_FIRST; r <= REG_LTCH_LAST; ++r)
			d->regs[r] = 0u;
		val = (uint8_t)(val & (uint8_t)~INT_CLK_CLR);
	}
	d->regs[reg] = val;
	d->write_count[reg]++;
	return 0;
}

static int
fake_tas2563_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_tas2563_data *d = target->data;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len < 2) return -EIO;
		/* Single-byte writes only (§7.3.6 Figure 7-6, p.33) -- the
		 * driver deliberately does not rely on the register address
		 * auto-incrementing, so neither does this fake. */
		if (msgs[0].len != 2) return -EIO;
		return apply_write(d, msgs[0].buf[0], msgs[0].buf[1]);
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len != 1 || msgs[1].len != 1) return -EIO;
		const uint8_t reg = msgs[0].buf[0];
		if (reg == REG_PAGE) {
			msgs[1].buf[0] = d->cur_page;
		} else if (reg == REG_BOOK) {
			msgs[1].buf[0] = d->cur_book;
		} else if (d->cur_book != 0u || d->cur_page != 0u) {
			msgs[1].buf[0] = 0u;
		} else {
			msgs[1].buf[0] = d->regs[reg];
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_tas2563_api = {
	.transfer = fake_tas2563_transfer,
};

static int fake_tas2563_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_tas2563_data *d = target->data;
	g_fake_tas2563              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_TAS2563_DEFINE(n) \
	static struct fake_tas2563_data fake_tas2563_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_tas2563_init, &fake_tas2563_data_##n, NULL, &fake_tas2563_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_TAS2563_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_tas2563_get_reg(uint8_t reg)
{
	return g_fake_tas2563 ? g_fake_tas2563->regs[reg] : 0u;
}

void fake_tas2563_set_reg(uint8_t reg, uint8_t val)
{
	if (g_fake_tas2563) g_fake_tas2563->regs[reg] = val;
}

uint32_t fake_tas2563_write_count(uint8_t reg)
{
	return g_fake_tas2563 ? g_fake_tas2563->write_count[reg] : 0u;
}

uint8_t fake_tas2563_cur_book(void)
{
	return g_fake_tas2563 ? g_fake_tas2563->cur_book : 0u;
}

uint8_t fake_tas2563_cur_page(void)
{
	return g_fake_tas2563 ? g_fake_tas2563->cur_page : 0u;
}

size_t fake_tas2563_log_len(void)
{
	return g_fake_tas2563 ? g_fake_tas2563->log_len : 0u;
}

const struct fake_tas2563_write *fake_tas2563_log(size_t i)
{
	if (g_fake_tas2563 == NULL || i >= g_fake_tas2563->log_len) return NULL;
	return &g_fake_tas2563->log[i];
}

void fake_tas2563_log_reset(void)
{
	if (g_fake_tas2563) g_fake_tas2563->log_len = 0u;
}

void fake_tas2563_fail_write_at(uint8_t book, uint8_t page, uint8_t reg)
{
	if (g_fake_tas2563 == NULL) return;
	g_fake_tas2563->fault_armed = true;
	g_fake_tas2563->fault_book  = book;
	g_fake_tas2563->fault_page  = page;
	g_fake_tas2563->fault_reg   = reg;
}

void fake_tas2563_reset(void)
{
	if (g_fake_tas2563) seed_defaults(g_fake_tas2563);
}
