/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1623: rv3028c7_dispatch_irq() used to write STATUS = 0x00
 * unconditionally after dispatch, clearing one-shot flags
 * (ALARM/EXT_EVENT/BSF) that latch between the STATUS read and the
 * acknowledge write -- silently losing the event and never invoking
 * that source's handler.  The fix reworked the acknowledge into a
 * write-0x00 + re-read step (rv3028_status_ack(),
 * chips/rv3028c7/rv3028c7.c) shared by rv3028c7_init(),
 * rv3028c7_alarm_check_and_clear() and rv3028c7_dispatch_irq(), so a
 * bit that races the acknowledge is reported back instead of
 * silently dropped.
 *
 * This links the REAL chips/rv3028c7/rv3028c7.c TU against a small
 * in-TU fake I2C register file (rv3028c7.c calls exactly
 * alp_i2c_write() / alp_i2c_write_read(); alp_i2c_t is opaque) so
 * the test exercises the actual dispatch/ack code path, not a
 * reimplementation of it -- a prior version of this test only
 * covered a since-removed pure bit-math helper and could not fail
 * at the driver's own defect site.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/ztest.h>

#include "alp/chips/rv3028c7.h"

/* STATUS / CONTROL_2 register addresses + STATUS bit positions,
 * mirrored from chips/rv3028c7/rv3028c7.c (private to that TU) so
 * this test doesn't need to expose them from the public header. */
#define REG_STATUS    0x0Eu
#define REG_CONTROL_2 0x10u
#define STATUS_PORF   0x01u
#define STATUS_AF     0x04u
#define STATUS_UF     0x10u
#define CONTROL_2_24H 0x40u

/* ------------------------------------------------------------------
 * Fake I2C register file.  rv3028c7.c's only bus dependency.
 * ------------------------------------------------------------------ */
static uint8_t fake_regs[0x40];
static uint8_t inject_bit;
static bool    inject_armed;

static void fake_reset(void)
{
	memset(fake_regs, 0, sizeof(fake_regs));
	inject_bit   = 0;
	inject_armed = false;
}

/* Arm a one-shot "this bit latches the instant the driver acks
 * STATUS" race: fires on the very next write to REG_STATUS, models
 * a source (e.g. ALARM) matching during the driver's own dispatch
 * or acknowledge round trip. */
static void fake_arm_race(uint8_t bit)
{
	inject_bit   = bit;
	inject_armed = true;
}

alp_status_t alp_i2c_write_read(alp_i2c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *tx,
                                size_t         txlen,
                                uint8_t       *rx,
                                size_t         rxlen)
{
	(void)bus;
	(void)addr;
	(void)txlen;
	uint8_t reg = tx[0];
	for (size_t i = 0; i < rxlen; ++i)
		rx[i] = fake_regs[(reg + i) & 0x3Fu];
	return ALP_OK;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)bus;
	(void)addr;
	uint8_t reg = data[0];
	for (size_t i = 1; i < len; ++i)
		fake_regs[(reg + i - 1) & 0x3Fu] = data[i];

	if (reg == REG_STATUS && inject_armed) {
		fake_regs[REG_STATUS] |= inject_bit;
		inject_armed = false;
	}
	return ALP_OK;
}

/* ------------------------------------------------------------------
 * Handler call counters.
 * ------------------------------------------------------------------ */
static int alarm_count;
static int periodic_count;

static void on_alarm(rv3028c7_t *ctx, rv3028c7_src_t src, void *user)
{
	(void)ctx;
	(void)src;
	(void)user;
	alarm_count++;
}

static void on_periodic(rv3028c7_t *ctx, rv3028c7_src_t src, void *user)
{
	(void)ctx;
	(void)src;
	(void)user;
	periodic_count++;
}

static rv3028c7_t open_rtc(void)
{
	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, (alp_i2c_t *)1), ALP_OK, "rv3028c7_init failed");
	return ctx;
}

ZTEST_SUITE(rv3028c7_dispatch_ack, NULL, NULL, NULL, NULL, NULL);

ZTEST(rv3028c7_dispatch_ack, test_init_clears_porf_and_forces_24h)
{
	fake_reset();
	fake_regs[REG_STATUS] = STATUS_PORF;

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, (alp_i2c_t *)1), ALP_OK, "init failed");
	zassert_equal(
	    fake_regs[REG_STATUS], 0, "PORF not cleared, regs[STATUS]=0x%02x", fake_regs[REG_STATUS]);
	zassert_true((fake_regs[REG_CONTROL_2] & CONTROL_2_24H) != 0, "24H bit not forced");
}

ZTEST(rv3028c7_dispatch_ack, test_dispatch_irq_dispatches_and_clears_observed_bit)
{
	fake_reset();
	fake_regs[REG_STATUS] = STATUS_UF;
	alarm_count = periodic_count = 0;

	rv3028c7_t ctx = open_rtc();
	rv3028c7_register_handler(&ctx, RV3028C7_SRC_PERIODIC, on_periodic, NULL);

	uint8_t      seen = 0xFF;
	alp_status_t s    = rv3028c7_dispatch_irq(&ctx, &seen);

	zassert_equal(s, ALP_OK, "dispatch_irq rc=%d", (int)s);
	zassert_equal(seen, STATUS_UF, "status_seen=0x%02x", seen);
	zassert_equal(periodic_count, 1, "periodic handler not invoked");
	zassert_equal(
	    fake_regs[REG_STATUS], 0, "STATUS not fully acked, regs=0x%02x", fake_regs[REG_STATUS]);
}

ZTEST(rv3028c7_dispatch_ack, test_dispatch_irq_survives_a_flag_that_latches_during_dispatch)
{
	/* This is the exact #1623 regression: STATUS reads only UF at
	 * dispatch time; ALARM (AF) latches while the acknowledge is in
	 * flight.  A correct dispatcher must still invoke the ALARM
	 * handler on this same call instead of clearing AF unseen. */
	fake_reset();
	fake_regs[REG_STATUS] = STATUS_UF;
	fake_arm_race(STATUS_AF);
	alarm_count = periodic_count = 0;

	rv3028c7_t ctx = open_rtc();
	rv3028c7_register_handler(&ctx, RV3028C7_SRC_PERIODIC, on_periodic, NULL);
	rv3028c7_register_handler(&ctx, RV3028C7_SRC_ALARM, on_alarm, NULL);

	alp_status_t s = rv3028c7_dispatch_irq(&ctx, NULL);

	zassert_equal(s, ALP_OK, "dispatch_irq rc=%d", (int)s);
	zassert_equal(periodic_count, 1, "periodic handler not invoked");
	zassert_equal(alarm_count,
	              1,
	              "ALARM handler not invoked -- the flag that latched during "
	              "dispatch was silently swallowed by the acknowledge (#1623)");
	zassert_equal(
	    fake_regs[REG_STATUS], 0, "STATUS not fully drained, regs=0x%02x", fake_regs[REG_STATUS]);
}

ZTEST(rv3028c7_dispatch_ack, test_alarm_check_and_clear_reports_a_race)
{
	/* Same race, narrower call site: AF has not latched at the
	 * initial read (only UF has), but latches while the ack for UF
	 * is in flight. *fired must still come back true.  Unlike
	 * dispatch_irq() (which loops to fully drain), this call site
	 * does a single ack pass, so the raced bit is reported but left
	 * latched in hardware -- it self-heals on the next call instead
	 * of being lost, which is the property under test. */
	fake_reset();
	fake_regs[REG_STATUS] = STATUS_UF;
	fake_arm_race(STATUS_AF);

	rv3028c7_t ctx = open_rtc();

	bool         fired = false;
	alp_status_t s     = rv3028c7_alarm_check_and_clear(&ctx, &fired);

	zassert_equal(s, ALP_OK, "alarm_check_and_clear rc=%d", (int)s);
	zassert_true(fired, "AF that latched during the ack round trip was not reported");
	zassert_equal(fake_regs[REG_STATUS] & STATUS_AF,
	              STATUS_AF,
	              "AF must remain latched in hardware (self-heal path), regs=0x%02x",
	              fake_regs[REG_STATUS]);
}
