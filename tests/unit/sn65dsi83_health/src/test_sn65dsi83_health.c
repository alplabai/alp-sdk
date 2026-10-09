/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SN65DSI83 auto-recovery decision.  Bench (E1M-AEN803, RVT121): the bridge reset itself to
 * its defaults mid-run -- CSR 0x0D 0x00, 0x0A 0x0A, 0xE5 0x3D -- while healthy reads
 * 0x01 / 0x81 / 0x00.  Exercised on the host against the pure decision the driver's
 * health pass and the bus-owning core's recovery agent both call; the I2C replay is not
 * covered here.
 */
#include <zephyr/ztest.h>

#include "sn65dsi83_recovery.h"

ZTEST_SUITE(sn65dsi83_health, NULL, NULL, NULL, NULL, NULL);

ZTEST(sn65dsi83_health, test_healthy)
{
	zassert_equal(sn65dsi83_health_decide(0x01, 0x81, 0x00), SN65_HEALTH_OK);
}

/* The bench failure: every register at its power-on value. */
ZTEST(sn65dsi83_health, test_bench_black_reinits)
{
	zassert_equal(sn65dsi83_health_decide(0x00, 0x0A, 0x3D), SN65_HEALTH_REINIT);
}

ZTEST(sn65dsi83_health, test_each_loss_reinits)
{
	/* PLL_EN cleared, rest healthy. */
	zassert_equal(sn65dsi83_health_decide(0x00, 0x81, 0x00), SN65_HEALTH_REINIT);
	/* PLL_EN set but PLL_EN_STAT clear: not locked. */
	zassert_equal(sn65dsi83_health_decide(0x01, 0x01, 0x00), SN65_HEALTH_REINIT);
	/* PLL up but HS_CLK_SRC clear: the table is gone. */
	zassert_equal(sn65dsi83_health_decide(0x01, 0x80, 0x00), SN65_HEALTH_REINIT);
}

/* The LVDS_CLK_RANGE bits (0x0A[3:1]) are panel facts, not health. */
ZTEST(sn65dsi83_health, test_clk_range_bits_ignored)
{
	zassert_equal(sn65dsi83_health_decide(0x01, 0x8B, 0x00), SN65_HEALTH_OK);
}

/* Transient sync errors with the PLL fine: clear and count, never re-init. */
ZTEST(sn65dsi83_health, test_error_bits_alone_clear_only)
{
	zassert_equal(sn65dsi83_health_decide(0x01, 0x81, 0x3D), SN65_HEALTH_CLEAR_ERRORS);
	zassert_equal(sn65dsi83_health_decide(0x01, 0x81, 0x01), SN65_HEALTH_CLEAR_ERRORS);
}

ZTEST(sn65dsi83_health, test_reinit_rate_limit)
{
	zassert_true(sn65dsi83_reinit_allowed(false, 0, 0), "the first re-init is never limited");
	zassert_false(sn65dsi83_reinit_allowed(true, 1000, 1000 + SN65_REINIT_MIN_GAP_MS - 1));
	zassert_true(sn65dsi83_reinit_allowed(true, 1000, 1000 + SN65_REINIT_MIN_GAP_MS));
}
