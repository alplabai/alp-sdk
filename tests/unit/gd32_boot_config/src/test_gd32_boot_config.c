/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32g553_boot_config_get / _set against a byte-level model of the bridge's
 * BOOT_CONFIG opcode (0x42).  The firmware queues a SET, replies at once with
 * the still-stored value, and commits from its main loop one flash erase
 * (~20 ms) later; the host must wait, then poll GET until the stored value
 * matches.  Covers the little-endian request encoding, the NOSUPPORT / BUSY
 * mapping, the poll-until-applied success path, the no-op SET (no poll) and
 * the never-applied timeout.
 *
 * The SPI bus and the alp_delay_* / alp_uptime_ms primitives are defined here
 * so the real gd32g553_init() + spi_xfer() run unmodified.
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
	uint32_t stored;         /* persisted boot-config flags */
	bool     pending;        /* a SET is queued, the main loop has not committed it yet */
	uint32_t pending_flags;
	uint64_t pending_at_ms;  /* when the SET was queued */
	uint64_t apply_after_ms; /* the commit lands this long after the SET */
	uint8_t  last_req[5];    /* last BOOT_CONFIG request payload */
	uint8_t  fw_status;      /* non-zero: answer every BOOT_CONFIG with this status */
} g_br;

static uint64_t g_now_ms;

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

	const uint8_t cmd = tx[1];
	g_br.exec[cmd]++;
	/* The firmware's main loop commits a queued SET once its flash erase is done. */
	if (g_br.pending && g_now_ms >= g_br.pending_at_ms + g_br.apply_after_ms) {
		g_br.stored  = g_br.pending_flags;
		g_br.pending = false;
	}
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
	case GD32G553_CMD_BOOT_CONFIG: {
		memcpy(g_br.last_req, &tx[2], sizeof(g_br.last_req));
		if (g_br.fw_status != 0u) {
			stage(g_br.fw_status, NULL, 0u); /* short error envelope */
			break;
		}
		const uint32_t flags = (uint32_t)tx[3] | ((uint32_t)tx[4] << 8) | ((uint32_t)tx[5] << 16) |
		                       ((uint32_t)tx[6] << 24);
		if (tx[2] == 1u && flags != g_br.stored && !g_br.pending) { /* SET: queue, never inline */
			g_br.pending       = true;
			g_br.pending_flags = flags;
			g_br.pending_at_ms = g_now_ms;
		}
		const uint8_t r[4] = { (uint8_t)g_br.stored, (uint8_t)(g_br.stored >> 8),
			                   (uint8_t)(g_br.stored >> 16), (uint8_t)(g_br.stored >> 24) };
		stage(0x00u, r, sizeof(r));
		break;
	}
	default:
		stage(0x00u, NULL, 0u);
		break;
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
	g_br.apply_after_ms = 25u; /* ~20 ms erase + main-loop latency */
	zassert_equal(gd32g553_init(&g_ctx, (alp_spi_t *)&g_spi_token, NULL, 0x70u), ALP_OK);
	memset(g_br.exec, 0, sizeof(g_br.exec));
}

ZTEST_SUITE(gd32_boot_config, NULL, NULL, each_before, NULL, NULL);

/* ---- tests --------------------------------------------------------------- */

/* The request is `op:u8 flags:u32` little-endian; the reply is decoded LE. */
ZTEST(gd32_boot_config, test_request_is_little_endian)
{
	uint32_t v = 0u;
	g_br.stored = 0x04030201u;
	zassert_equal(gd32g553_boot_config_get(&g_ctx, &v), ALP_OK);
	zassert_equal(v, 0x04030201u, "reply decoded little-endian");
	zassert_equal(g_br.last_req[0], 0u, "op GET");

	/* SET of the value the model already stores: one request, no polling. */
	zassert_equal(gd32g553_boot_config_set(&g_ctx, 0x04030201u), ALP_OK);
	const uint8_t want[5] = { 1u, 0x01u, 0x02u, 0x03u, 0x04u };
	zassert_mem_equal(g_br.last_req, want, sizeof want);
}

ZTEST(gd32_boot_config, test_nosupport_maps_to_alp_err_nosupport)
{
	uint32_t v;
	g_br.fw_status = 0x06u; /* STATUS_NOSUPPORT */
	zassert_equal(gd32g553_boot_config_get(&g_ctx, &v), ALP_ERR_NOSUPPORT);
	zassert_equal(
	    gd32g553_boot_config_set(&g_ctx, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH), ALP_ERR_NOSUPPORT);
	zassert_equal(g_br.exec[GD32G553_CMD_BOOT_CONFIG], 2u, "no polling after a refused SET");
}

ZTEST(gd32_boot_config, test_busy_is_returned_without_polling)
{
	g_br.fw_status = 0x03u; /* STATUS_BUSY: a different SET is still being committed */
	zassert_equal(
	    gd32g553_boot_config_set(&g_ctx, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH), ALP_ERR_BUSY);
	zassert_equal(g_br.exec[GD32G553_CMD_BOOT_CONFIG], 1u);
}

/* The core of the redesign: the SET reply carries the OLD value, the commit
 * lands ~25 ms later, and set() returns OK only once GET shows it. */
ZTEST(gd32_boot_config, test_set_polls_until_the_value_is_applied)
{
	const uint64_t t0 = g_now_ms;
	zassert_equal(gd32g553_boot_config_set(&g_ctx, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH), ALP_OK);
	zassert_equal(g_br.stored, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH, "stored by the model");
	zassert_false(g_br.pending);
	zassert_true(g_br.exec[GD32G553_CMD_BOOT_CONFIG] >= 2u, "SET plus at least one GET poll");
	zassert_true(g_now_ms - t0 >= 25u, "host waited out the flash erase before returning");
	zassert_true(g_now_ms - t0 < 100u, "and did not wait much longer than needed");
}

ZTEST(gd32_boot_config, test_noop_set_does_not_poll)
{
	g_br.stored       = GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH;
	const uint64_t t0 = g_now_ms;
	zassert_equal(gd32g553_boot_config_set(&g_ctx, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH), ALP_OK);
	zassert_equal(g_br.exec[GD32G553_CMD_BOOT_CONFIG], 1u, "one request, no GET");
	zassert_true(g_now_ms - t0 < 10u, "no 30 ms wait for a no-op SET");
}

/* An accepted SET whose commit never lands (flash fault in the firmware's
 * main loop) must not read as success. */
ZTEST(gd32_boot_config, test_set_that_never_applies_times_out)
{
	g_br.apply_after_ms = UINT64_MAX / 2u;
	zassert_equal(gd32g553_boot_config_set(&g_ctx, GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH),
	              ALP_ERR_TIMEOUT);
	zassert_equal(g_br.stored, 0u);
}
