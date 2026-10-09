/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SN65DSI83 auto-recovery.  Bench (E1M-AEN803, RVT121): the bridge reset itself to its
 * defaults mid-run -- CSR 0x0D 0x00, 0x0A 0x0A, 0xE5 0x3D -- while healthy reads
 * 0x01 / 0x81 / 0x00.  Exercised on the host: the pure decision, and
 * sn65dsi83_health_poll() against a fake I2C register file (replay, rate limit, a failed
 * read, a failed replay, the write-1-to-clear write-back).  A replay into a live video
 * stream is silicon territory and is not covered here.
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>

#include "sn65dsi83_recovery.h"

/* ---- the decision ---------------------------------------------------------------------- */

ZTEST_SUITE(sn65dsi83_decide, NULL, NULL, NULL, NULL, NULL);

#define D(pll, clk, err, polls, after) sn65dsi83_health_decide(pll, clk, err, 1U, polls, after)

ZTEST(sn65dsi83_decide, test_healthy)
{
	zassert_equal(D(0x01, 0x81, 0x00, 0, false), SN65_HEALTH_OK);
}

ZTEST(sn65dsi83_decide, test_bench_black_reinits)
{
	zassert_equal(D(0x00, 0x0A, 0x3D, 1, false), SN65_HEALTH_REINIT);
}

ZTEST(sn65dsi83_decide, test_each_loss_reinits)
{
	zassert_equal(D(0x00, 0x81, 0x00, 0, false), SN65_HEALTH_REINIT, "PLL_EN clear");
	zassert_equal(D(0x01, 0x01, 0x00, 0, false), SN65_HEALTH_REINIT, "PLL not locked");
	zassert_equal(D(0x01, 0x80, 0x00, 0, false), SN65_HEALTH_REINIT, "HS_CLK_SRC lost");
}

/* The expected HS_CLK_SRC comes from the table: a table that writes 0 there is healthy with 0. */
ZTEST(sn65dsi83_decide, test_hs_clk_src_follows_the_table)
{
	zassert_equal(sn65dsi83_health_decide(0x01, 0x80, 0x00, 0U, 0, false), SN65_HEALTH_OK);
	zassert_equal(sn65dsi83_health_decide(0x01, 0x81, 0x00, 0U, 0, false), SN65_HEALTH_REINIT);
}

ZTEST(sn65dsi83_decide, test_clk_range_bits_ignored)
{
	zassert_equal(D(0x01, 0x8B, 0x00, 0, false), SN65_HEALTH_OK);
}

/* One transient sync error with the PLL fine: clear and count, never re-init. */
ZTEST(sn65dsi83_decide, test_single_error_clears_only)
{
	zassert_equal(D(0x01, 0x81, 0x3D, 1, false), SN65_HEALTH_CLEAR_ERRORS);
	zassert_equal(D(0x01, 0x81, 0x3D, 2, false), SN65_HEALTH_CLEAR_ERRORS);
}

/* Errors that persist, or that are there on the first look after a re-init, are not transient. */
ZTEST(sn65dsi83_decide, test_persistent_errors_reinit)
{
	zassert_equal(D(0x01, 0x81, 0x3D, SN65_ERR_POLLS_REINIT, false), SN65_HEALTH_REINIT);
	zassert_equal(D(0x01, 0x81, 0x01, 1, true), SN65_HEALTH_REINIT);
	zassert_equal(D(0x01, 0x81, 0x00, 0, true), SN65_HEALTH_OK, "clean after a re-init is fine");
}

ZTEST(sn65dsi83_decide, test_reinit_rate_limit)
{
	zassert_true(sn65dsi83_reinit_allowed(false, 0, 0), "the first re-init is never limited");
	zassert_false(sn65dsi83_reinit_allowed(true, 1000, 1000 + SN65_REINIT_MIN_GAP_MS - 1));
	zassert_true(sn65dsi83_reinit_allowed(true, 1000, 1000 + SN65_REINIT_MIN_GAP_MS));
}

/* ---- health_poll against a fake bridge -------------------------------------------------- */

#define WRITE_LOG_MAX 64

static struct {
	uint8_t regs[256];
	struct {
		uint8_t reg;
		uint8_t val;
	} wlog[WRITE_LOG_MAX];
	int  nwrites;
	bool fail_reads;     /* every read NAKs (bridge unpowered / EN low) */
	int  fail_write_reg; /* a write to this CSR NAKs; -1 = none */
} fake;

static int fake_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num, uint16_t addr)
{
	ARG_UNUSED(dev);
	zassert_equal(addr, 0x2c);

	if (num == 2 && (msgs[0].flags & I2C_MSG_READ) == 0 && msgs[0].len == 1 &&
	    (msgs[1].flags & I2C_MSG_READ) != 0) {
		if (fake.fail_reads) {
			return -EIO;
		}
		for (uint32_t i = 0; i < msgs[1].len; i++) {
			msgs[1].buf[i] = fake.regs[(uint8_t)(msgs[0].buf[0] + i)];
		}
		return 0;
	}

	zassert_equal(num, 1);
	zassert_equal(msgs[0].len, 2U, "single-byte CSR writes only");

	uint8_t reg = msgs[0].buf[0];
	uint8_t val = msgs[0].buf[1];

	if (fake.nwrites < WRITE_LOG_MAX) {
		fake.wlog[fake.nwrites].reg = reg;
		fake.wlog[fake.nwrites].val = val;
	}
	fake.nwrites++;
	if (fake.fail_write_reg == reg) {
		return -EIO;
	}

	if (reg == 0xE5) { /* write-1-to-clear */
		fake.regs[reg] &= (uint8_t)~val;
	} else if (reg == 0x0D) { /* PLL_EN drives PLL_EN_STAT */
		fake.regs[reg]  = val;
		fake.regs[0x0A] = (val & 1U) ? (fake.regs[0x0A] | 0x80U) : (fake.regs[0x0A] & 0x7FU);
	} else if (reg == 0x0A) { /* PLL_EN_STAT is read-only */
		fake.regs[reg] = (val & 0x7FU) | (fake.regs[reg] & 0x80U);
	} else {
		fake.regs[reg] = val;
	}
	return 0;
}

static const struct i2c_driver_api fake_api = { .transfer = fake_transfer };
DEVICE_DEFINE(fake_i2c, "fake_i2c", NULL, NULL, NULL, NULL, POST_KERNEL, 99, &fake_api);

static const struct i2c_dt_spec bus = { .bus = DEVICE_GET(fake_i2c), .addr = 0x2c };

/* A small table is enough: the replay writes whatever it is given. */
static const struct sn65dsi83_csr table[] = {
	{ 0x0A, 0x01 }, /* LVDS_CLK_RANGE 0, HS_CLK_SRC 1 */
	{ 0x0B, 0x28 },
	{ 0x18, 0x78 },
	{ 0x20, 0x00 },
};

static struct sn65dsi83_stats  st;
static struct sn65dsi83_report rep;

static void black(void)
{
	fake.regs[0x0D] = 0x00;
	fake.regs[0x0A] = 0x0A;
	fake.regs[0xE5] = 0x3D;
}

static void healthy(void)
{
	fake.regs[0x0D] = 0x01;
	fake.regs[0x0A] = 0x81;
	fake.regs[0xE5] = 0x00;
}

static void before_each(void *unused)
{
	/* ID, CSR 0x00..0x08 (datasheet Table 7-4, read ascending). */
	static const uint8_t id[] = { 0x35, 0x38, 0x49, 0x53, 0x44, 0x20, 0x20, 0x20, 0x01 };

	ARG_UNUSED(unused);
	memset(&fake, 0, sizeof(fake));
	fake.fail_write_reg = -1;
	memcpy(fake.regs, id, sizeof(id));
	memset(&st, 0, sizeof(st));
	memset(&rep, 0, sizeof(rep));
	healthy();
}

static int poll(int64_t now_ms)
{
	return sn65dsi83_health_poll(&bus, table, ARRAY_SIZE(table), &st, &rep, now_ms);
}

ZTEST_SUITE(sn65dsi83_poll, NULL, NULL, before_each, NULL, NULL);

ZTEST(sn65dsi83_poll, test_healthy_touches_nothing)
{
	zassert_equal(poll(0), 0);
	zassert_equal(rep.health, SN65_HEALTH_OK);
	zassert_equal(fake.nwrites, 0);
}

/* The bench state: replay from PLL off, whole table, PLL back, errors cleared. */
ZTEST(sn65dsi83_poll, test_black_state_replays)
{
	black();
	zassert_equal(poll(1000), 0);
	zassert_equal(rep.health, SN65_HEALTH_REINIT);
	zassert_equal(rep.err, 0);
	zassert_equal(st.recoveries, 1U);
	zassert_equal(st.failures, 0U);

	zassert_equal(fake.wlog[0].reg, 0x0D, "the replay starts from PLL off");
	zassert_equal(fake.wlog[0].val, 0x00);
	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		zassert_equal(fake.wlog[1 + i].reg, table[i].reg, "table in order");
		zassert_equal(fake.wlog[1 + i].val, table[i].val);
	}
	zassert_equal(fake.regs[0x0D], 0x01, "PLL_EN set again");
	zassert_equal(fake.regs[0x0A], 0x81, "locked, HS_CLK_SRC kept");
	zassert_equal(fake.regs[0xE5], 0x00, "errors cleared");
}

ZTEST(sn65dsi83_poll, test_rate_limit_then_replay_again)
{
	black();
	zassert_equal(poll(1000), 0);
	zassert_equal(st.recoveries, 1U);

	black();
	fake.nwrites = 0;
	zassert_equal(poll(1000 + SN65_REINIT_MIN_GAP_MS - 1), 0);
	zassert_true(rep.suppressed);
	zassert_equal(fake.nwrites, 0, "no writes inside the window");
	zassert_equal(st.recoveries, 1U);

	zassert_equal(poll(1000 + SN65_REINIT_MIN_GAP_MS), 0);
	zassert_false(rep.suppressed);
	zassert_equal(st.recoveries, 2U, "replays again after the window");
	zassert_equal(fake.regs[0x0D], 0x01);
}

ZTEST(sn65dsi83_poll, test_read_failure_takes_no_action)
{
	black();
	fake.fail_reads = true;
	zassert_not_equal(poll(1000), 0);
	zassert_equal(fake.nwrites, 0);
	zassert_equal(st.recoveries + st.failures + st.errors_cleared, 0U);
}

/* A replay that dies half-way leaves PLL off, so the next pass sees a bridge to replay. */
ZTEST(sn65dsi83_poll, test_mid_replay_failure_writes_pll_off)
{
	black();
	fake.fail_write_reg = 0x18;
	zassert_equal(poll(1000), 0);
	zassert_not_equal(rep.err, 0);
	zassert_equal(st.failures, 1U);
	zassert_equal(st.recoveries, 0U);
	zassert_true(fake.nwrites > 0 && fake.nwrites <= WRITE_LOG_MAX);
	zassert_equal(fake.wlog[fake.nwrites - 1].reg, 0x0D);
	zassert_equal(fake.wlog[fake.nwrites - 1].val, 0x00, "PLL_EN cleared after the failure");
	zassert_equal(fake.regs[0x0D], 0x00);

	/* The next window retries and succeeds. */
	fake.fail_write_reg = -1;
	zassert_equal(poll(1000 + SN65_REINIT_MIN_GAP_MS), 0);
	zassert_equal(st.recoveries, 1U);
}

/* A wrong chip answering at the address is never written to. */
ZTEST(sn65dsi83_poll, test_wrong_id_is_not_written)
{
	black();
	fake.regs[0x00] = 0x00;
	zassert_equal(poll(1000), 0);
	zassert_equal(st.failures, 1U);
	for (int i = 0; i < fake.nwrites; i++) {
		zassert_true(fake.wlog[i].reg == 0x0D && fake.wlog[i].val == 0x00,
		             "only the PLL-off write");
	}
}

ZTEST(sn65dsi83_poll, test_error_bits_write_back_what_was_read)
{
	fake.regs[0xE5] = 0x25;
	zassert_equal(poll(1000), 0);
	zassert_equal(rep.health, SN65_HEALTH_CLEAR_ERRORS);
	zassert_equal(fake.nwrites, 1);
	zassert_equal(fake.wlog[0].reg, 0xE5);
	zassert_equal(fake.wlog[0].val, 0x25, "W1C: the value read is the value written back");
	zassert_equal(st.errors_cleared, 1U);
	zassert_equal(st.recoveries, 0U);
	zassert_equal(fake.regs[0xE5], 0x00);
}

/* Errors that come back on three polls in a row are not transient. */
ZTEST(sn65dsi83_poll, test_persistent_errors_escalate)
{
	for (int i = 0; i < (int)SN65_ERR_POLLS_REINIT - 1; i++) {
		fake.regs[0xE5] = 0x3D;
		zassert_equal(poll(1000 + i * 1000), 0);
		zassert_equal(rep.health, SN65_HEALTH_CLEAR_ERRORS);
	}
	fake.regs[0xE5] = 0x3D;
	zassert_equal(poll(1000 + 2 * 1000), 0);
	zassert_equal(rep.health, SN65_HEALTH_REINIT);
	zassert_equal(st.recoveries, 1U);
}

/* A clean poll in between resets the streak. */
ZTEST(sn65dsi83_poll, test_clean_poll_resets_the_streak)
{
	fake.regs[0xE5] = 0x3D;
	zassert_equal(poll(1000), 0);
	fake.regs[0xE5] = 0x3D;
	zassert_equal(poll(2000), 0);
	zassert_equal(poll(3000), 0); /* clean */
	fake.regs[0xE5] = 0x3D;
	zassert_equal(poll(4000), 0);
	zassert_equal(rep.health, SN65_HEALTH_CLEAR_ERRORS);
	zassert_equal(st.recoveries, 0U);
}

/* Errors on the first poll after a re-init mean the replay did not hold the link. */
ZTEST(sn65dsi83_poll, test_errors_right_after_a_recovery_escalate)
{
	black();
	zassert_equal(poll(1000), 0);
	zassert_equal(st.recoveries, 1U);

	fake.regs[0xE5] = 0x01;
	zassert_equal(poll(1000 + SN65_REINIT_MIN_GAP_MS), 0);
	zassert_equal(rep.health, SN65_HEALTH_REINIT);
	zassert_equal(st.recoveries, 2U);
}

/* ---- the bus owner's latched recipe ------------------------------------------------------- */

static struct sn65dsi83_recipe recipe;
static struct sn65dsi83_latch  latch;

static void publish(const struct sn65dsi83_csr *t, uint8_t n)
{
	memset(&recipe, 0, sizeof(recipe));
	memcpy((void *)recipe.csr, t, n * sizeof(*t));
	recipe.n     = n;
	recipe.magic = SN65_RECIPE_MAGIC;
}

ZTEST(sn65dsi83_poll, test_no_recipe_latches_nothing)
{
	memset(&recipe, 0, sizeof(recipe));
	memset(&latch, 0, sizeof(latch));
	zassert_false(sn65dsi83_recipe_latch(&recipe, &latch));
	zassert_equal(latch.n, 0U);
}

/* A warm reboot of the display core clears the magic and may never publish again (its re-init can
 * lose the race for the bus it handed away): the bus owner keeps its copy and still recovers. */
ZTEST(sn65dsi83_poll, test_latched_recipe_survives_a_cleared_magic)
{
	memset(&latch, 0, sizeof(latch));
	publish(table, ARRAY_SIZE(table));
	zassert_true(sn65dsi83_recipe_latch(&recipe, &latch));
	zassert_equal(latch.n, ARRAY_SIZE(table));

	recipe.magic = 0U; /* the display core restarted */
	zassert_false(sn65dsi83_recipe_latch(&recipe, &latch), "nothing newer");
	zassert_equal(latch.n, ARRAY_SIZE(table), "the latched table stays in use");

	black();
	zassert_equal(sn65dsi83_health_poll(&bus, latch.csr, latch.n, &st, &rep, 1000), 0);
	zassert_equal(rep.health, SN65_HEALTH_REINIT);
	zassert_equal(st.recoveries, 1U, "a lost config is still replayed");
	zassert_equal(fake.regs[0x0D], 0x01);
	zassert_equal(fake.regs[0x18], 0x78, "from the latched table");
}

/* A later valid recipe replaces the latched one; a half-written one is ignored. */
ZTEST(sn65dsi83_poll, test_newer_recipe_refreshes_the_latch)
{
	static const struct sn65dsi83_csr other[] = { { 0x0A, 0x01 }, { 0x18, 0x7A } };

	memset(&latch, 0, sizeof(latch));
	publish(table, ARRAY_SIZE(table));
	zassert_true(sn65dsi83_recipe_latch(&recipe, &latch));

	publish(other, ARRAY_SIZE(other));
	zassert_true(sn65dsi83_recipe_latch(&recipe, &latch));
	zassert_equal(latch.n, ARRAY_SIZE(other));
	zassert_equal(latch.csr[1].val, 0x7A);

	publish(table, ARRAY_SIZE(table));
	recipe.n = SN65_RECIPE_MAX + 1U; /* garbage length */
	zassert_false(sn65dsi83_recipe_latch(&recipe, &latch));
	zassert_equal(latch.n, ARRAY_SIZE(other), "untouched");
}

/* ---- the bus owner rides out an I2C transient ------------------------------------------------ */

/* A failing poll (the display core's restart, a camera transfer in flight) changes nothing; the
 * bridge is judged again the moment the bus answers. */
ZTEST(sn65dsi83_poll, test_transient_io_error_then_replay)
{
	uint32_t streak = 0U;

	black();
	fake.fail_reads = true;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(1000)), "one failure is a retry");
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(2000)));
	zassert_equal(fake.nwrites, 0, "no write while the bus does not answer");

	fake.fail_reads = false;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(3000)));
	zassert_equal(streak, 0U, "an answered poll ends the streak");
	zassert_equal(rep.health, SN65_HEALTH_REINIT);
	zassert_equal(st.recoveries, 1U, "the replay happens as soon as the bus is back");
}

/* The streak reports once per episode (SN65_IO_FAIL_POLLS failures in a row) and re-arms after a
 * success; nothing is written to the bus while it fails. */
ZTEST(sn65dsi83_poll, test_persistent_io_error_reports_once_per_episode)
{
	uint32_t streak  = 0U;
	unsigned reports = 0U;

	fake.fail_reads = true;
	for (int i = 1; i <= 3 * SN65_IO_FAIL_POLLS; i++) {
		if (sn65dsi83_io_fail_step(&streak, poll(1000 * i))) {
			reports++;
			zassert_equal(i, SN65_IO_FAIL_POLLS, "on the Nth failure in a row, once");
		}
	}
	zassert_equal(reports, 1U);
	zassert_equal(fake.nwrites, 0, "the bus is only retried, never written while it fails");

	/* A good poll ends the episode: a later wedge is reported again. */
	fake.fail_reads = false;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(100000)));
	zassert_equal(streak, 0U);
	fake.fail_reads = true;
	for (int i = 1; i <= 2 * SN65_IO_FAIL_POLLS; i++) {
		if (sn65dsi83_io_fail_step(&streak, poll(200000 + 1000 * i))) {
			reports++;
		}
	}
	zassert_equal(reports, 2U);

	/* A bus that fails twice, answers, fails twice: never reaches the threshold. */
	streak          = 0U;
	fake.fail_reads = true;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(0)));
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(1000)));
	fake.fail_reads = false;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(2000)));
	fake.fail_reads = true;
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(3000)));
	zassert_false(sn65dsi83_io_fail_step(&streak, poll(4000)));
}

/* ---- what a display core may touch at boot ---------------------------------------------------- */

ZTEST_SUITE(sn65dsi83_boot, NULL, NULL, NULL, NULL, NULL);

/* Cold: the whole sequence, and the owner's record starts clean. */
ZTEST(sn65dsi83_boot, test_cold_runs_everything)
{
	struct sn65dsi83_boot_plan p = sn65dsi83_boot_plan_for(false, true);

	zassert_true(p.toggle_en && p.touch_bus && p.clear_recipe && p.publish_recipe);
}

/* Warm (the bus owner runs): EN stays up, the bus is not touched, the recipe and counters are
 * kept, the recipe is published again. */
ZTEST(sn65dsi83_boot, test_warm_leaves_bus_and_en_alone)
{
	struct sn65dsi83_boot_plan p = sn65dsi83_boot_plan_for(true, true);

	zassert_false(p.toggle_en, "EN must not go low: the bridge is being watched");
	zassert_false(p.touch_bus, "no ID check, no CSR write, no controller access");
	zassert_false(p.clear_recipe, "the owner's latch and counters stay valid");
	zassert_true(p.publish_recipe);
}

/* A core that keeps its own bus (no recipe) has no owner to leave it to: warm makes no difference. */
ZTEST(sn65dsi83_boot, test_no_recipe_is_always_a_full_init)
{
	struct sn65dsi83_boot_plan w = sn65dsi83_boot_plan_for(true, false);
	struct sn65dsi83_boot_plan c = sn65dsi83_boot_plan_for(false, false);

	zassert_true(w.toggle_en && w.touch_bus);
	zassert_false(w.clear_recipe || w.publish_recipe);
	zassert_equal(memcmp(&w, &c, sizeof(w)), 0);
}
