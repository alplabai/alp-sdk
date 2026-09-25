/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Renesas DA9292 i2c-emul target (7-bit 0x1E).  Stateful, so the
 * CH2 DEEPX-rail sequence can run end to end:
 *   - POR image = the V2N OTP variant as the bench reads it: DEV_ID
 *     0xEA, REV_ID 0x42, CFG_REV 0x11, CTRL_01 0x81 (CH2_VSTEP=1,
 *     CH1_EN=1), CH2 VSEL_LO/HI 0xB4/0x9A (1.80 V / 1.54 V at VSTEP=1),
 *     CH1 VSEL 0xA0 (0.800 V), STATUS_00 = CH1_PG.
 *   - CTRL_01: a CHx_VSTEP change is ignored while CHx_EN is 1 BEFORE
 *     the write (the datasheet only allows VSTEP to move with the
 *     channel off).  CH2_EN 0->1 arms CH2_PG to assert on the Nth
 *     STATUS_00 read (fake_da9292_set_pg_delay; UINT32_MAX = never);
 *     CH2_EN 1->0 drops CH2_PG.
 *   - EVENT_00/01 are write-1-to-clear; STATUS and ID are read-only.
 * Every write (applied or ignored) is logged in order, so a test can
 * assert what the driver ASKED for, e.g. "no CTRL_01 write ever cleared
 * CH2_EN".
 */

#define DT_DRV_COMPAT alp_fake_da9292

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

#define REG_STATUS_00 0x00u
#define REG_STATUS_01 0x01u
#define REG_EVENT_00  0x02u
#define REG_EVENT_01  0x03u
#define REG_CTRL_01   0x07u

#define CTRL_CH2_VSTEP 0x80u
#define CTRL_CH1_VSTEP 0x40u
#define CTRL_CH2_EN    0x02u
#define CTRL_CH1_EN    0x01u
#define STATUS_CH2_PG  0x02u
#define STATUS_CH1_PG  0x01u

static uint8_t                  g_regs[256];
static uint32_t                 g_write_count[256];
static struct fake_da9292_write g_log[FAKE_DA9292_LOG_MAX];
static size_t                   g_log_len;
static uint32_t                 g_pg_delay;     /* STATUS_00 reads until CH2_PG */
static uint32_t                 g_pg_countdown; /* live countdown while pending */
static bool                     g_pg_pending;

static void write_ctrl_01(uint8_t val)
{
	const uint8_t old = g_regs[REG_CTRL_01];
	uint8_t       nv  = val;

	/* VSTEP only moves while its channel is off. */
	if ((old & CTRL_CH2_EN) != 0u)
		nv = (uint8_t)((nv & (uint8_t)~CTRL_CH2_VSTEP) | (old & CTRL_CH2_VSTEP));
	if ((old & CTRL_CH1_EN) != 0u)
		nv = (uint8_t)((nv & (uint8_t)~CTRL_CH1_VSTEP) | (old & CTRL_CH1_VSTEP));

	if ((old & CTRL_CH2_EN) == 0u && (nv & CTRL_CH2_EN) != 0u) {
		g_pg_pending   = g_pg_delay != UINT32_MAX;
		g_pg_countdown = g_pg_delay;
	} else if ((old & CTRL_CH2_EN) != 0u && (nv & CTRL_CH2_EN) == 0u) {
		g_pg_pending = false;
		g_regs[REG_STATUS_00] &= (uint8_t)~STATUS_CH2_PG;
	}
	g_regs[REG_CTRL_01] = nv;
}

static void write_reg(uint8_t reg, uint8_t val)
{
	g_write_count[reg]++;
	if (g_log_len < FAKE_DA9292_LOG_MAX)
		g_log[g_log_len++] = (struct fake_da9292_write){ reg, val };

	switch (reg) {
	case REG_STATUS_00:
	case REG_STATUS_01:
	case 0x19u: /* DEV_ID */
	case 0x1Au: /* REV_ID */
	case 0x1Bu: /* CFG_REV */
		break;  /* read-only */
	case REG_EVENT_00:
	case REG_EVENT_01:
		g_regs[reg] &= (uint8_t)~val; /* W1C */
		break;
	case REG_CTRL_01:
		write_ctrl_01(val);
		break;
	default:
		g_regs[reg] = val;
		break;
	}
}

static uint8_t read_reg(uint8_t reg)
{
	if (reg == REG_STATUS_00 && g_pg_pending) {
		if (g_pg_countdown == 0u) {
			g_regs[REG_STATUS_00] |= STATUS_CH2_PG;
			g_pg_pending = false;
		} else {
			g_pg_countdown--;
		}
	}
	return g_regs[reg];
}

static int
fake_da9292_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)target;
	(void)addr;

	if (num_msgs == 1 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		if (msgs[0].len < 2) return -EIO;
		for (uint32_t i = 1; i < msgs[0].len; i++) {
			write_reg((uint8_t)(msgs[0].buf[0] + i - 1u), msgs[0].buf[i]);
		}
		return 0;
	}
	if (num_msgs == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (msgs[0].len < 1) return -EIO;
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			msgs[1].buf[i] = read_reg((uint8_t)(msgs[0].buf[0] + i));
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api fake_da9292_api = {
	.transfer = fake_da9292_transfer,
};

static int fake_da9292_init(const struct emul *target, const struct device *parent)
{
	(void)target;
	(void)parent;
	fake_da9292_reset();
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for
 * why this bare placeholder device is needed. */
#define FAKE_DA9292_DEFINE(n) \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_da9292_init, NULL, NULL, &fake_da9292_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_DA9292_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

uint8_t fake_da9292_get_reg(uint8_t reg)
{
	return g_regs[reg];
}

void fake_da9292_force_reg(uint8_t reg, uint8_t val)
{
	g_regs[reg] = val;
}

uint32_t fake_da9292_write_count(uint8_t reg)
{
	return g_write_count[reg];
}

size_t fake_da9292_log_len(void)
{
	return g_log_len;
}

const struct fake_da9292_write *fake_da9292_log(size_t i)
{
	return i < g_log_len ? &g_log[i] : NULL;
}

void fake_da9292_log_reset(void)
{
	g_log_len = 0;
	memset(g_write_count, 0, sizeof g_write_count);
}

void fake_da9292_set_pg_delay(uint32_t reads)
{
	g_pg_delay = reads;
}

void fake_da9292_reset(void)
{
	memset(g_regs, 0, sizeof g_regs);
	g_regs[REG_STATUS_00] = STATUS_CH1_PG;
	g_regs[REG_CTRL_01]   = CTRL_CH2_VSTEP | CTRL_CH1_EN;
	g_regs[0x0Au]         = 0xA0u; /* CH1 VSEL_LO 0.800 V */
	g_regs[0x0Bu]         = 0xA0u; /* CH1 VSEL_HI */
	g_regs[0x0Cu]         = 0xB4u; /* CH2 VSEL_LO 1.80 V at VSTEP=1 */
	g_regs[0x0Du]         = 0x9Au; /* CH2 VSEL_HI 1.54 V at VSTEP=1 */
	g_regs[0x19u]         = 0xEAu;
	g_regs[0x1Au]         = 0x42u;
	g_regs[0x1Bu]         = 0x11u;
	g_pg_delay            = 2u;
	g_pg_pending          = false;
	g_pg_countdown        = 0u;
	fake_da9292_log_reset();
}
