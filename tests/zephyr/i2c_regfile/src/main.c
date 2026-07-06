/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/i2c_regfile.h> -- register-file I2C target helper tests.
 *
 * Two build flavours (see testcase.yaml):
 *
 *   - default (CONFIG_I2C_TARGET=y): native_sim's emulated controller
 *     forwards same-bus transfers addressed to the registered target
 *     to its callbacks, so controller-side alp_i2c_write /
 *     alp_i2c_write_read calls drive the helper's ISR state machine
 *     end to end -- a REAL loopback, no external hardware.
 *
 *   - notarget (CONFIG_I2C_TARGET=n): the controller driver's
 *     target_register is NULL, alp_i2c_target_open fails with
 *     ALP_ERR_NOSUPPORT, and the helper must propagate exactly that.
 *
 * The param-validation tests are flavour-independent: they reject
 * before the wrapped target open is ever attempted.
 */

#include <zephyr/ztest.h>

#include "alp/i2c_regfile.h"
#include "alp/peripheral.h"

#define RF_BUS_ID    0u
#define RF_ADDR      0x42u
#define RF_REG_COUNT 8u

static volatile uint8_t g_regs[RF_REG_COUNT];

ZTEST_SUITE(alp_i2c_regfile, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Param validation -- rejected before any bus interaction, so these   */
/* hold in both build flavours.                                        */
/* ------------------------------------------------------------------ */

ZTEST(alp_i2c_regfile, test_open_null_out_yields_inval)
{
	alp_status_t s = alp_i2c_regfile_open(RF_BUS_ID, RF_ADDR, g_regs, RF_REG_COUNT, NULL);
	zassert_equal(s, ALP_ERR_INVAL, "got %d", (int)s);
}

ZTEST(alp_i2c_regfile, test_open_null_regs_yields_inval)
{
	alp_i2c_regfile_t *rf = (alp_i2c_regfile_t *)0x1;
	alp_status_t       s  = alp_i2c_regfile_open(RF_BUS_ID, RF_ADDR, NULL, RF_REG_COUNT, &rf);
	zassert_equal(s, ALP_ERR_INVAL, "got %d", (int)s);
	zassert_is_null(rf, "out must be NULLed on failure");
}

ZTEST(alp_i2c_regfile, test_open_zero_len_yields_inval)
{
	alp_i2c_regfile_t *rf = NULL;
	alp_status_t       s  = alp_i2c_regfile_open(RF_BUS_ID, RF_ADDR, g_regs, 0u, &rf);
	zassert_equal(s, ALP_ERR_INVAL, "got %d", (int)s);
	zassert_is_null(rf, "no handle on failure");
}

ZTEST(alp_i2c_regfile, test_open_reserved_address_yields_inval)
{
	alp_i2c_regfile_t *rf = NULL;

	/* 0x00..0x07 (general call etc.) and 0x78..0x7F are reserved;
	 * the documented valid range is 0x08..0x77. */
	alp_status_t s = alp_i2c_regfile_open(RF_BUS_ID, 0x00u, g_regs, RF_REG_COUNT, &rf);
	zassert_equal(s, ALP_ERR_INVAL, "addr 0x00: got %d", (int)s);

	s = alp_i2c_regfile_open(RF_BUS_ID, 0x07u, g_regs, RF_REG_COUNT, &rf);
	zassert_equal(s, ALP_ERR_INVAL, "addr 0x07: got %d", (int)s);

	s = alp_i2c_regfile_open(RF_BUS_ID, 0x78u, g_regs, RF_REG_COUNT, &rf);
	zassert_equal(s, ALP_ERR_INVAL, "addr 0x78: got %d", (int)s);
}

ZTEST(alp_i2c_regfile, test_window_and_stats_null_handle_yield_inval)
{
	alp_i2c_regfile_stats_t st;

	zassert_equal(alp_i2c_regfile_set_write_window(NULL, 0u, 1u), ALP_ERR_INVAL, "window");
	zassert_equal(alp_i2c_regfile_stats(NULL, &st), ALP_ERR_INVAL, "stats null rf");
}

ZTEST(alp_i2c_regfile, test_close_null_is_noop)
{
	alp_i2c_regfile_close(NULL); /* must not crash */
}

#ifdef CONFIG_I2C_TARGET

/* ------------------------------------------------------------------ */
/* Loopback -- the emul controller routes controller-side transfers    */
/* on the same bus into the registered target's callbacks.             */
/* ------------------------------------------------------------------ */

/* Open helper + controller side, prime regs with 0xA0.. pattern. */
static void rf_loopback_begin(alp_i2c_regfile_t **rf, alp_i2c_t **bus)
{
	for (uint8_t i = 0; i < RF_REG_COUNT; i++) {
		g_regs[i] = (uint8_t)(0xA0 + i);
	}
	alp_status_t s = alp_i2c_regfile_open(RF_BUS_ID, RF_ADDR, g_regs, RF_REG_COUNT, rf);
	zassert_equal(s, ALP_OK, "regfile open: got %d", (int)s);
	zassert_not_null(*rf, "handle");

	*bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = RF_BUS_ID,
	    .bitrate_hz = 100000,
	});
	zassert_not_null(*bus, "controller open");
}

static void rf_loopback_end(alp_i2c_regfile_t *rf, alp_i2c_t *bus)
{
	alp_i2c_close(bus);
	alp_i2c_regfile_close(rf);
}

ZTEST(alp_i2c_regfile, test_loopback_write_then_readback)
{
	alp_i2c_regfile_t *rf;
	alp_i2c_t         *bus;
	rf_loopback_begin(&rf, &bus);

	/* Controller write: pointer byte 0x02 + two payload bytes. */
	alp_status_t s = alp_i2c_write(bus, RF_ADDR, (uint8_t[]){ 0x02, 0x55, 0x66 }, 3);
	zassert_equal(s, ALP_OK, "write: got %d", (int)s);
	zassert_equal(g_regs[2], 0x55, "payload byte 0 stored at latched pointer");
	zassert_equal(g_regs[3], 0x66, "payload byte 1 auto-incremented");

	/* Repeated-START read: pointer byte 0x01, then stream 3 bytes.
	 * STOP at the end of the previous write re-armed the latch. */
	uint8_t rbuf[3] = { 0 };
	s               = alp_i2c_write_read(bus, RF_ADDR, (uint8_t[]){ 0x01 }, 1, rbuf, 3);
	zassert_equal(s, ALP_OK, "write_read: got %d", (int)s);
	zassert_equal(rbuf[0], 0xA1, "reg 1");
	zassert_equal(rbuf[1], 0x55, "reg 2 (just written)");
	zassert_equal(rbuf[2], 0x66, "reg 3 (just written)");

	rf_loopback_end(rf, bus);
}

ZTEST(alp_i2c_regfile, test_loopback_pointer_wraparound)
{
	alp_i2c_regfile_t *rf;
	alp_i2c_t         *bus;
	rf_loopback_begin(&rf, &bus);

	/* Write two bytes starting at the last register: the second
	 * must wrap to register 0, EEPROM-style. */
	alp_status_t s = alp_i2c_write(bus, RF_ADDR, (uint8_t[]){ RF_REG_COUNT - 1u, 0x11, 0x22 }, 3);
	zassert_equal(s, ALP_OK, "write: got %d", (int)s);
	zassert_equal(g_regs[RF_REG_COUNT - 1u], 0x11, "last register");
	zassert_equal(g_regs[0], 0x22, "wrapped to register 0");

	/* Pointer latch wraps modulo len too: 0x0A % 8 == 2. */
	uint8_t rbuf[1] = { 0 };
	s               = alp_i2c_write_read(bus, RF_ADDR, (uint8_t[]){ 0x0A }, 1, rbuf, 1);
	zassert_equal(s, ALP_OK, "write_read: got %d", (int)s);
	zassert_equal(rbuf[0], g_regs[2], "latch taken modulo len");

	rf_loopback_end(rf, bus);
}

ZTEST(alp_i2c_regfile, test_loopback_write_window_drops_readonly_writes)
{
	alp_i2c_regfile_t *rf;
	alp_i2c_t         *bus;
	rf_loopback_begin(&rf, &bus);

	/* Registers 0..1 read-only, 2..7 writable. */
	alp_status_t s = alp_i2c_regfile_set_write_window(rf, 2u, RF_REG_COUNT - 2u);
	zassert_equal(s, ALP_OK, "window: got %d", (int)s);

	/* Write across the boundary: bytes for regs 1 (dropped) and 2
	 * (stored) -- the pointer must advance through dropped writes
	 * like real silicon with a read-only ID register. */
	s = alp_i2c_write(bus, RF_ADDR, (uint8_t[]){ 0x01, 0xDE, 0xAD }, 3);
	zassert_equal(s, ALP_OK, "write: got %d", (int)s);
	zassert_equal(g_regs[1], 0xA1, "read-only register unchanged");
	zassert_equal(g_regs[2], 0xAD, "pointer advanced past dropped write");

	/* Window that does not fit the file is rejected. */
	s = alp_i2c_regfile_set_write_window(rf, RF_REG_COUNT, 1u);
	zassert_equal(s, ALP_ERR_INVAL, "oversized window: got %d", (int)s);

	rf_loopback_end(rf, bus);
}

ZTEST(alp_i2c_regfile, test_loopback_stats_count_traffic)
{
	alp_i2c_regfile_t *rf;
	alp_i2c_t         *bus;
	rf_loopback_begin(&rf, &bus);

	alp_i2c_regfile_stats_t st = { 0xFF, 0xFF };
	zassert_equal(alp_i2c_regfile_stats(rf, &st), ALP_OK, "stats");
	zassert_equal(st.writes_seen, 0u, "fresh handle: no writes");
	zassert_equal(st.reads_seen, 0u, "fresh handle: no reads");

	/* 1 pointer byte (not counted) + 2 payload bytes; then a
	 * 3-byte read stream. */
	(void)alp_i2c_write(bus, RF_ADDR, (uint8_t[]){ 0x00, 0x01, 0x02 }, 3);
	uint8_t rbuf[3];
	(void)alp_i2c_write_read(bus, RF_ADDR, (uint8_t[]){ 0x00 }, 1, rbuf, 3);

	zassert_equal(alp_i2c_regfile_stats(rf, &st), ALP_OK, "stats");
	zassert_equal(st.writes_seen, 2u, "payload bytes only, got %u", st.writes_seen);
	zassert_equal(st.reads_seen, 3u, "streamed bytes, got %u", st.reads_seen);

	/* Stats on a closed handle degrade to INVAL, not stale data. */
	rf_loopback_end(rf, bus);
	zassert_equal(alp_i2c_regfile_stats(rf, &st), ALP_ERR_INVAL, "closed handle");
}

#else /* !CONFIG_I2C_TARGET */

/* ------------------------------------------------------------------ */
/* Degrade path -- the helper must fail exactly as the raw target      */
/* open does when the controller driver lacks target mode.             */
/* ------------------------------------------------------------------ */

ZTEST(alp_i2c_regfile, test_open_without_target_support_yields_nosupport)
{
	alp_i2c_regfile_t *rf = (alp_i2c_regfile_t *)0x1;
	alp_status_t       s  = alp_i2c_regfile_open(RF_BUS_ID, RF_ADDR, g_regs, RF_REG_COUNT, &rf);
	zassert_equal(s, ALP_ERR_NOSUPPORT, "got %d", (int)s);
	zassert_is_null(rf, "no handle on NOSUPPORT");
}

#endif /* CONFIG_I2C_TARGET */
