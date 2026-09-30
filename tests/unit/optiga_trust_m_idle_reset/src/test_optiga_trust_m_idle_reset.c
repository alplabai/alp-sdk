/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for alplabai/alp-sdk#2507: after more than about 10 s idle
 * the Trust M NACKs every register-address write for as long as the host
 * keeps polling, and only a hardware reset (SE_RST) revives it.
 * optiga_trust_m_init_with_reset() must, once the NACK-polling budget is
 * spent, pulse RESET (low for the library's RESET_LOW_TIME_MSEC, then wait
 * STARTUP_TIME_MSEC) and probe once more -- and report NOT_READY only when
 * the part is still silent afterwards.
 *
 * The part is modelled at the alp_i2c level: while `wedged` every
 * transfer NACKs; a release edge after an assert clears it.  Time is a
 * fake clock advanced by alp_delay_ms so the pulse widths are asserted.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/chips/optiga_trust_m.h"
#include "ifx_i2c_config.h"

static struct {
	bool     wedged;         /* NACK everything until a reset pulse */
	bool     survives_reset; /* false: part stays silent after a pulse */
	bool     asserted;
	unsigned writes, reads;
	unsigned asserts, releases;
	uint64_t assert_ms, low_ms, release_ms, first_probe_after_ms;
	bool     probed_since_release;
	bool     fail_assert;
} g;

static uint64_t g_now_ms;

void alp_delay_ms(uint32_t ms)
{
	g_now_ms += ms;
}

uint64_t alp_uptime_ms(void)
{
	return g_now_ms;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)bus;
	(void)addr;
	(void)data;
	(void)len;
	g.writes++;
	if (g.releases != 0u && !g.probed_since_release) {
		g.probed_since_release = true;
		g.first_probe_after_ms = g_now_ms - g.release_ms;
	}
	return g.wedged ? ALP_ERR_IO : ALP_OK;
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
	(void)bus;
	(void)addr;
	g.reads++;
	if (g.wedged) return ALP_ERR_IO;
	memset(data, 0, len);
	data[0] = 0x08u;
	return ALP_OK;
}

static alp_status_t reset_hook(void *user, bool assert)
{
	zassert_equal(user, &g, "user cookie is passed through");
	if (assert) {
		g.asserts++;
		g.assert_ms = g_now_ms;
		g.asserted  = true;
		return g.fail_assert ? ALP_ERR_IO : ALP_OK;
	}
	g.releases++;
	g.release_ms = g_now_ms;
	g.low_ms     = g_now_ms - g.assert_ms;
	if (g.asserted && g.survives_reset) g.wedged = false;
	g.asserted = false;
	return ALP_OK;
}

static alp_i2c_t *const bus = (alp_i2c_t *)0x1;

static void *setup(void)
{
	return NULL;
}

static void before(void *unused)
{
	(void)unused;
	memset(&g, 0, sizeof(g));
	g.survives_reset = true;
	g_now_ms         = 0u;
}

ZTEST_SUITE(optiga_idle_reset, NULL, setup, before, NULL, NULL);

ZTEST(optiga_idle_reset, test_healthy_part_is_never_reset)
{
	optiga_trust_m_t ctx;

	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	zassert_equal(g.asserts, 0u);
	zassert_equal(g.writes, 1u);
	zassert_equal(g.reads, 1u);
}

ZTEST(optiga_idle_reset, test_wedged_part_is_revived_by_one_reset_pulse)
{
	optiga_trust_m_t ctx;

	g.wedged = true;
	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	zassert_true(ctx.initialised);
	/* The whole NACK-polling budget was spent before resetting... */
	zassert_equal(g.writes, PL_POLLING_MAX_CNT + 1u);
	/* ...then exactly one pulse, of the library's timings. */
	zassert_equal(g.asserts, 1u);
	zassert_equal(g.releases, 1u);
	zassert_true(g.low_ms >= RESET_LOW_TIME_MSEC / 1000u, "RESET low %llu ms", g.low_ms);
	zassert_true(g.first_probe_after_ms >= (STARTUP_TIME_MSEC + 999u) / 1000u,
	             "first probe %llu ms after release",
	             g.first_probe_after_ms);
}

ZTEST(optiga_idle_reset, test_silent_after_reset_reads_absent_after_a_single_pulse)
{
	optiga_trust_m_t ctx;

	g.wedged         = true;
	g.survives_reset = false;
	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_ERR_NOT_READY);
	zassert_false(ctx.initialised);
	zassert_equal(g.asserts, 1u, "one reset, not a loop");
	zassert_equal(g.writes, 2u * PL_POLLING_MAX_CNT);
}

ZTEST(optiga_idle_reset, test_failed_assert_still_releases_and_skips_the_second_probe)
{
	optiga_trust_m_t ctx;

	g.wedged      = true;
	g.fail_assert = true;
	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_ERR_NOT_READY);
	zassert_equal(g.releases, 1u, "RESET is never left asserted");
	zassert_equal(g.writes, PL_POLLING_MAX_CNT);
}

ZTEST(optiga_idle_reset, test_no_hook_is_the_plain_probe)
{
	optiga_trust_m_t ctx;

	g.wedged = true;
	zassert_equal(optiga_trust_m_init(&ctx, bus, 0), ALP_ERR_NOT_READY);
	zassert_equal(g.writes, PL_POLLING_MAX_CNT);
	zassert_equal(g.asserts, 0u);
}

/* #2517: the part idles out AFTER a good init.  The mock cannot speak the
 * IFX framing, so a reopen cannot succeed here; what is asserted is the
 * policy: one reset pulse through the hook kept from init, a second open
 * attempt after it, and none of that without a hook. */
ZTEST(optiga_idle_reset, test_mid_app_open_failure_pulses_reset_once_and_reopens)
{
	optiga_trust_m_t              ctx;
	optiga_trust_m_product_info_t info;

	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	zassert_equal(g.asserts, 0u);

	g.wedged         = true; /* idled out after init */
	g.survives_reset = false;
	zassert_not_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_OK);
	zassert_equal(g.asserts, 1u, "one reset, not a loop");
	zassert_equal(g.releases, 1u, "RESET is released");
	zassert_true(g.probed_since_release, "opened again after the reset");
	zassert_true(g.first_probe_after_ms >= (STARTUP_TIME_MSEC + 999u) / 1000u,
	             "start-up wait before the reopen");
	optiga_trust_m_deinit(&ctx);
}

ZTEST(optiga_idle_reset, test_mid_app_open_failure_without_hook_is_not_reset)
{
	optiga_trust_m_t              ctx;
	optiga_trust_m_product_info_t info;

	zassert_equal(optiga_trust_m_init(&ctx, bus, 0), ALP_OK);
	g.wedged = true;
	zassert_not_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_OK);
	zassert_equal(g.asserts, 0u);
	optiga_trust_m_deinit(&ctx);
}

/* A timed-out op leaves the library op in flight.  The next call must
 * report busy and must not pulse RESET over a part that may be fine.  The
 * mock cannot make the library time out, so the in-flight state is set
 * directly (op_pending 1 = open of the util session, op_status 0x0001 =
 * OPTIGA_LIB_BUSY, which never completes here). */
ZTEST(optiga_idle_reset, test_op_still_in_flight_is_busy_not_reset)
{
	optiga_trust_m_t              ctx;
	optiga_trust_m_product_info_t info;

	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	ctx.op_pending = 1u;
	ctx.op_status  = 0x0001u;
	zassert_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_BUSY);
	zassert_equal(g.asserts, 0u, "no reset while the op is in flight");
	zassert_equal(ctx.op_pending, 1u, "still pending");
	optiga_trust_m_deinit(&ctx);
}

/* A timed-out op that has since completed is drained, then the call goes
 * on as a normal open: on a part that idled out that is one reset pulse.
 * op_pending 3 = an op that left no session behind; op_status 0x0000 is
 * OPTIGA_LIB_SUCCESS, 0x0002 a failed op. */
ZTEST(optiga_idle_reset, test_completed_op_is_drained_then_open_proceeds)
{
	optiga_trust_m_t              ctx;
	optiga_trust_m_product_info_t info;

	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	g.wedged         = true;
	g.survives_reset = false;
	ctx.op_pending   = 3u;
	ctx.op_status    = 0x0000u;
	zassert_not_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_BUSY);
	zassert_equal(ctx.op_pending, 0u, "drained");
	zassert_equal(g.asserts, 1u, "then the normal open ran, one reset");
	optiga_trust_m_deinit(&ctx);
}

ZTEST(optiga_idle_reset, test_failed_op_is_drained_then_open_proceeds)
{
	optiga_trust_m_t              ctx;
	optiga_trust_m_product_info_t info;

	zassert_equal(optiga_trust_m_init_with_reset(&ctx, bus, 0, reset_hook, &g), ALP_OK);
	g.wedged         = true;
	g.survives_reset = false;
	ctx.op_pending   = 3u;
	ctx.op_status    = 0x0002u;
	zassert_not_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_BUSY);
	zassert_equal(ctx.op_pending, 0u, "drained");
	zassert_equal(g.asserts, 1u, "then the normal open ran, one reset");
	optiga_trust_m_deinit(&ctx);
}
