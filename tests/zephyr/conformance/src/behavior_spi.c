/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing SPI virtual backend
 * (epic #610 §2). Compiled only for the alp_sdk.conformance.
 * test_doubles twister scenario (CONFIG_ALP_SDK_TESTING=y) -- see
 * testcase.yaml. Drives the double through the PUBLIC alp/peripheral.h
 * API plus the alp/testing injection surface; never touches
 * spi_ops.h / testing_drv.c internals directly.
 *
 * Built alongside src/behavior_gpio.c, src/behavior_uart.c and
 * src/behavior_i2c.c in this scenario's app image (this scenario's
 * CMakeLists swaps all of these in for src/main.c) -- see the top of
 * behavior_gpio.c for why main.c's per-class expectations are
 * incompatible with a priority-255 "open ANY instance" test double and
 * must never share a binary with it.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/testing/common.h>
#include <alp/testing/spi.h>

/* Local copy of main.c's / behavior_gpio.c's enum-membership helper --
 * every behavior_*.c is static to its own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static alp_spi_t *open_bus(uint32_t bus_id)
{
	alp_spi_config_t cfg = ALP_SPI_CONFIG_DEFAULT(bus_id);
	return alp_spi_open(&cfg);
}

static void spi_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
}

/* After-each teardown, mirroring behavior_uart.c's / behavior_i2c.c's
 * dispatcher pool-health check: alp_testing_reset_all() wipes the
 * testing double's own state but cannot reach the DISPATCHER's private
 * static handle pool (CONFIG_ALP_SDK_MAX_SPI_HANDLES slots,
 * src/spi_dispatch.c). A test that leaks a handle would otherwise
 * silently shrink the pool for every later test until it quietly runs
 * out, surfacing as a confusing ALP_ERR_NOMEM far from the actual
 * leak. Round-tripping a fresh handle here fails loudly instead. */
static void spi_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_spi_t *h = open_bus(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_spi_open(0) returned NULL right after "
	                 "this test -- a prior test in this file leaked a handle out of the "
	                 "dispatcher's fixed-size pool");
	alp_spi_close(h);
}

ZTEST_SUITE(alp_testing_spi_behavior, NULL, NULL, spi_behavior_before, spi_behavior_after, NULL);

/* Setup-fixture-shaped assertion (mirrors behavior_gpio.c's / behavior_uart.c's
 * / behavior_i2c.c's): a mis-selection must fail LOUDLY, not silently
 * exercise the wrong backend for every other case below. */
ZTEST(alp_testing_spi_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("spi", ALP_SOC_REF_STR);

	zassert_not_null(be, "spi class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "spi backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_SPI "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "spi backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

/* Backend-selection assertion beyond the fixture's own check --
 * exercised again explicitly per the task's deliverable list. */
ZTEST(alp_testing_spi_behavior, test_backend_vendor_is_alp_testing)
{
	const alp_backend_t *be = alp_backend_select("spi", ALP_SOC_REF_STR);

	zassert_not_null(be, "spi class has no registered backend at all");
	zassert_equal(strcmp(be->vendor, "alp_testing"), 0, "vendor must be alp_testing");
}

ZTEST(alp_testing_spi_behavior, test_write_captures_last_mosi)
{
	const uint32_t bus_id = 30;
	const uint8_t  msg[]  = { 0x01, 0x02, 0x03 };

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");

	zassert_equal(alp_spi_write(h, msg, sizeof(msg)), ALP_OK, "write() failed");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(
	    alp_testing_spi_last_mosi(bus_id, out, sizeof(out), &got), ALP_OK, "last_mosi failed");
	zassert_equal(got, sizeof(msg), "last_mosi returned %zu, expected %zu", got, sizeof(msg));
	zassert_mem_equal(out, msg, sizeof(msg), "last_mosi did not observe the written bytes");

	alp_spi_close(h);
}

ZTEST(alp_testing_spi_behavior, test_read_returns_canned_miso)
{
	const uint32_t bus_id = 31;
	const uint8_t  miso[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_equal(
	    alp_testing_spi_load_miso(bus_id, miso, sizeof(miso)), ALP_OK, "load_miso failed");

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");

	uint8_t buf[sizeof(miso)] = { 0 };
	zassert_equal(alp_spi_read(h, buf, sizeof(buf)), ALP_OK, "read() failed");
	zassert_mem_equal(buf, miso, sizeof(miso), "read() did not observe the canned MISO bytes");

	/* Canned MISO is a persistent snapshot, not a drained queue: a
	 * second read observes the same data. */
	uint8_t buf2[sizeof(miso)] = { 0 };
	zassert_equal(alp_spi_read(h, buf2, sizeof(buf2)), ALP_OK, "second read() failed");
	zassert_mem_equal(buf2, miso, sizeof(miso), "second read() must observe the same canned data");

	alp_spi_close(h);
}

ZTEST(alp_testing_spi_behavior, test_transceive_is_full_duplex)
{
	const uint32_t bus_id = 32;
	const uint8_t  tx[]   = { 0x10, 0x20, 0x30 };
	const uint8_t  miso[] = { 0x91, 0x92, 0x93 };

	zassert_equal(
	    alp_testing_spi_load_miso(bus_id, miso, sizeof(miso)), ALP_OK, "load_miso failed");

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");

	uint8_t rx[sizeof(tx)] = { 0 };
	zassert_equal(alp_spi_transceive(h, tx, rx, sizeof(tx)), ALP_OK, "transceive() failed");
	zassert_mem_equal(rx, miso, sizeof(miso), "transceive() did not observe the canned MISO bytes");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(
	    alp_testing_spi_last_mosi(bus_id, out, sizeof(out), &got), ALP_OK, "last_mosi failed");
	zassert_equal(got, sizeof(tx), "last_mosi must capture the MOSI half of the transceive");
	zassert_mem_equal(out, tx, sizeof(tx), "last_mosi did not observe the transceive's tx bytes");

	alp_spi_close(h);
}

/* NACK: documented status (ALP_ERR_IO), zero bytes transferred either
 * direction. */
ZTEST(alp_testing_spi_behavior, test_nack_returns_documented_status)
{
	const uint32_t bus_id = 33;
	const uint8_t  msg[]  = { 0x11, 0x22 };

	zassert_equal(
	    alp_testing_spi_fail_next(bus_id, ALP_TESTING_FAULT_NACK, 0), ALP_OK, "fail_next failed");

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");

	alp_status_t s = alp_spi_write(h, msg, sizeof(msg));
	zassert_equal(
	    s, ALP_ERR_IO, "write() must surface the documented bus-fault status (ALP_ERR_IO)");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	uint8_t out[8] = { 0 };
	size_t  got    = 1; /* poison to prove last_mosi zeroes it */
	zassert_equal(
	    alp_testing_spi_last_mosi(bus_id, out, sizeof(out), &got), ALP_OK, "last_mosi failed");
	zassert_equal(got, 0, "a NACKed transceive must capture zero MOSI bytes");

	/* One-shot: the fault disarms itself, so the next write succeeds. */
	zassert_equal(alp_spi_write(h, msg, sizeof(msg)),
	              ALP_OK,
	              "fail_next must be one-shot -- the following transceive must succeed");

	alp_spi_close(h);
}

/* Short transfer: only `short_len` bytes actually transferred in
 * either direction, then the documented bus-error status. */
ZTEST(alp_testing_spi_behavior, test_short_transfer_partial)
{
	const uint32_t bus_id    = 34;
	const uint8_t  tx[]      = { 0xDE, 0xAD, 0xBE, 0xEF };
	const uint8_t  miso[]    = { 0x01, 0x02, 0x03, 0x04 };
	const size_t   short_len = 2;

	zassert_equal(
	    alp_testing_spi_load_miso(bus_id, miso, sizeof(miso)), ALP_OK, "load_miso failed");
	zassert_equal(alp_testing_spi_fail_next(bus_id, ALP_TESTING_FAULT_SHORT, short_len),
	              ALP_OK,
	              "fail_next failed");

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");

	uint8_t rx[sizeof(tx)] = { 0xFF, 0xFF, 0xFF, 0xFF };
	zassert_equal(alp_spi_transceive(h, tx, rx, sizeof(tx)),
	              ALP_ERR_IO,
	              "short transceive must surface ALP_ERR_IO");
	zassert_mem_equal(rx, miso, short_len, "short transceive filled the wrong MISO bytes");
	zassert_equal(rx[short_len], 0, "bytes beyond short_len must be zero-padded, not left stale");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(
	    alp_testing_spi_last_mosi(bus_id, out, sizeof(out), &got), ALP_OK, "last_mosi failed");
	zassert_equal(got, short_len, "a short transceive must capture exactly short_len MOSI bytes");
	zassert_mem_equal(out, tx, short_len, "short transceive captured the wrong MOSI bytes");

	alp_spi_close(h);
}

/* Reset-frees-side-state regression: prime a canned MISO stream,
 * capture a MOSI write, arm a fault -- then reset_all() and prove the
 * bus is fully reusable with none of that state carried over. */
ZTEST(alp_testing_spi_behavior, test_reset_all_frees_side_state)
{
	const uint32_t bus_id = 35;
	const uint8_t  miso[] = { 0x9A };
	const uint8_t  wr[]   = { 0x01 };

	zassert_equal(
	    alp_testing_spi_load_miso(bus_id, miso, sizeof(miso)), ALP_OK, "load_miso failed");
	zassert_equal(
	    alp_testing_spi_fail_next(bus_id, ALP_TESTING_FAULT_NACK, 0), ALP_OK, "fail_next failed");

	alp_spi_t *h = open_bus(bus_id);
	zassert_not_null(h, "spi test double must open ANY instance");
	/* Consume the armed NACK first so it doesn't leak into the
	 * reset_all() assertions below. */
	zassert_equal(alp_spi_write(h, wr, sizeof(wr)), ALP_ERR_IO, "expected the armed NACK");
	alp_spi_close(h);

	alp_testing_reset_all();

	/* last_mosi must report "never touched" again (ALP_ERR_INVAL),
	 * not the pre-reset capture -- checked BEFORE anything re-touches
	 * bus_id, since last_mosi itself is a pure lookup that must not
	 * implicitly re-create the slot. */
	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(alp_testing_spi_last_mosi(bus_id, out, sizeof(out), &got),
	              ALP_ERR_INVAL,
	              "reset_all() must have cleared last_mosi's side-state for this bus");

	h = open_bus(bus_id);
	zassert_not_null(h, "bus must be reusable after reset_all()");

	/* Canned MISO must be gone: a fresh read returns zeros, not the
	 * pre-reset miso[]. */
	uint8_t buf[sizeof(miso)] = { 0xFF };
	zassert_equal(alp_spi_read(h, buf, sizeof(buf)), ALP_OK, "read() failed");
	zassert_equal(buf[0], 0, "reset_all() must have cleared the canned MISO bytes");

	alp_spi_close(h);
}
