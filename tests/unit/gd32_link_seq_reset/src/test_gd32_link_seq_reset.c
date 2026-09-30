/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression: a GD32 reset (OTA commit/rollback, watchdog) turns the
 * v0.7 STATUS_SEQ feature off, so every reply the bridge sends afterwards
 * is stamped 0.  The host used to keep seq_enabled/seq_last, read those
 * stamp-0 replies as "stale", RE-SENT the request (which the bridge had
 * in fact executed -- twice for a non-idempotent opcode) and then failed
 * with ALP_ERR_IO.  The driver must instead recognise the reset
 * signature, never re-send, re-negotiate LINK_FEATURES and fail that one
 * call.
 *
 * The bridge is modelled at the SPI byte level (alp_spi_write/read are
 * defined here, so the real gd32g553_init() + spi_xfer() run unmodified):
 * it CRC-checks each request, counts every decode per opcode, and stamps
 * replies with a 4-bit counter while the feature is on.  A "reset" clears
 * the feature + counter, exactly what the firmware does on reboot.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/chips/gd32g553.h"
#include "alp/protocol/crc16.h"

/* ---- simulated bridge ---------------------------------------------------- */

#define SOF 0xA5u

static struct {
	bool     stamping;       /* STATUS_SEQ granted on the slave */
	uint8_t  counter;        /* 4-bit stamp counter */
	uint32_t exec[256];      /* decodes per opcode (execution count) */
	uint8_t  staged[16];     /* reply staged for the next read */
	size_t   staged_len;     /* 0 = nothing staged */
	bool     drop_next_req;  /* slave "never decodes" the next request */
	bool     reset_after_op; /* slave reboots right after the next decode */
} g_br;

static uint64_t g_now_ms;

static void bridge_reset(void)
{
	g_br.stamping = false;
	g_br.counter  = 0u;
}

static void stage(uint8_t code, const uint8_t *payload, size_t n)
{
	uint8_t stamp = 0u;
	if (g_br.stamping) {
		g_br.counter = (uint8_t)((g_br.counter + 1u) & 0x0Fu);
		stamp        = g_br.counter;
	}
	g_br.staged[0] = SOF;
	g_br.staged[1] = (uint8_t)((stamp << 4) | code);
	if (n > 0u) memcpy(&g_br.staged[2], payload, n);
	const uint16_t crc      = alp_crc16_ccitt_false(g_br.staged, 2u + n);
	g_br.staged[2u + n]     = (uint8_t)(crc & 0xFFu);
	g_br.staged[2u + n + 1] = (uint8_t)(crc >> 8);
	g_br.staged_len         = 2u + n + 2u;
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)
{
	(void)bus;
	if (len < 4u || tx[0] != SOF) return ALP_ERR_IO;
	const uint16_t crc = alp_crc16_ccitt_false(tx, len - 2u);
	if ((uint16_t)(tx[len - 2u] | (tx[len - 1u] << 8)) != crc) return ALP_OK; /* dropped */
	if (g_br.drop_next_req) {
		g_br.drop_next_req = false;
		return ALP_OK; /* old staged reply stays armed */
	}

	const uint8_t cmd = tx[1];
	g_br.exec[cmd]++;
	switch (cmd) {
	case GD32G553_CMD_GET_VERSION: {
		const uint8_t v[3] = { 0u, 12u, 0u };
		stage(0x00u, v, sizeof(v));
		break;
	}
	case GD32G553_CMD_LINK_FEATURES: {
		g_br.stamping   = (tx[2] & GD32G553_LINK_FEAT_STATUS_SEQ) != 0u;
		const uint8_t g = g_br.stamping ? GD32G553_LINK_FEAT_STATUS_SEQ : 0u;
		stage(0x00u, &g, 1u);
		break;
	}
	default: /* PING, GPIO_WRITE, OTA_COMMIT, ...: empty OK reply */
		stage(0x00u, NULL, 0u);
		break;
	}
	if (g_br.reset_after_op) {
		g_br.reset_after_op = false;
		bridge_reset();
	}
	return ALP_OK;
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)
{
	(void)bus;
	memset(rx, 0, len);
	if (g_br.staged_len > 0u) memcpy(rx, g_br.staged, MIN(len, g_br.staged_len));
	return ALP_OK; /* the reply is re-served until the next decode */
}

alp_status_t
alp_i2c_write_read(alp_i2c_t *b, uint8_t a, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
	(void)b;
	(void)a;
	(void)w;
	(void)wl;
	(void)r;
	(void)rl;
	return ALP_ERR_NOSUPPORT;
}

void alp_delay_us(uint32_t us)
{
	(void)us;
}

void alp_delay_ms(uint32_t ms)
{
	g_now_ms += ms;
}

uint64_t alp_uptime_ms(void)
{
	return g_now_ms += 1u;
}

/* ---- fixture ------------------------------------------------------------- */

static gd32g553_t g_ctx;
static int        g_spi_token;

static void each_before(void *unused)
{
	(void)unused;
	memset(&g_br, 0, sizeof(g_br));
	memset(&g_ctx, 0, sizeof(g_ctx));
	/* Bring the link up with STATUS_SEQ granted, then advance the stamp
	 * to a mid-range non-zero baseline (seq_last = 4). */
	zassert_equal(gd32g553_init(&g_ctx, (alp_spi_t *)&g_spi_token, NULL, 0x70u), ALP_OK);
	zassert_true(g_ctx.seq_enabled);
	for (int i = 0; i < 3; i++) {
		zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 1u), ALP_OK);
	}
	zassert_equal(g_ctx.seq_last, 4u);
	memset(g_br.exec, 0, sizeof(g_br.exec));
}

ZTEST_SUITE(gd32_link_seq_reset, NULL, NULL, each_before, NULL, NULL);

/* ---- tests --------------------------------------------------------------- */

/* The bug: after a slave reset the next non-idempotent call must execute
 * ONCE (never re-sent), fail loudly, and leave a working, re-negotiated
 * link. */
ZTEST(gd32_link_seq_reset, test_reset_never_double_executes)
{
	bridge_reset(); /* GD32 rebooted: feature off, replies stamped 0 */

	zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 1u), ALP_ERR_IO);
	zassert_equal(g_br.exec[GD32G553_CMD_GPIO_WRITE], 1u, "request was re-sent (executed twice)");

	/* Re-negotiated: sequencing is back on against the slave's new counter. */
	zassert_true(g_ctx.seq_enabled);
	zassert_true(g_br.stamping);

	/* The link is usable again and each call still executes exactly once. */
	zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 0u), ALP_OK);
	zassert_equal(g_br.exec[GD32G553_CMD_GPIO_WRITE], 2u);
}

/* OTA commit/rollback reset the bridge: the driver must drop its
 * sequencing state itself so the next call is not misread. */
ZTEST(gd32_link_seq_reset, test_ota_commit_drops_seq_state)
{
	g_br.reset_after_op = true;
	zassert_equal(gd32g553_ota_commit(&g_ctx), ALP_OK);
	zassert_false(g_ctx.seq_enabled, "seq state kept across an OTA reset");

	zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 1u), ALP_OK);
	zassert_equal(g_br.exec[GD32G553_CMD_GPIO_WRITE], 1u);
}

/* Guard against over-triggering: a genuine stale re-serve (slave never
 * decoded the request, still stamping) is still re-sent exactly once. */
ZTEST(gd32_link_seq_reset, test_genuine_stale_is_still_resent_once)
{
	g_br.drop_next_req = true;
	zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 1u), ALP_OK);
	zassert_equal(g_br.exec[GD32G553_CMD_GPIO_WRITE], 1u);
	zassert_equal(g_ctx.seq_stale_count, 1u);
}

/* Guard against over-triggering: the 4-bit stamp legitimately wraps
 * 0xF -> 0, which must not read as a reset. */
ZTEST(gd32_link_seq_reset, test_stamp_wrap_is_not_a_reset)
{
	for (int i = 0; i < 40; i++) {
		zassert_equal(gd32g553_gpio_write(&g_ctx, 1u, 1u), ALP_OK, "call %d", i);
	}
	zassert_equal(g_br.exec[GD32G553_CMD_GPIO_WRITE], 40u);
	zassert_equal(g_ctx.seq_stale_count, 0u);
}
