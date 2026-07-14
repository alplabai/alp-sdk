/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing I2C virtual backend
 * (epic #610 §2). Compiled only for the alp_sdk.conformance.
 * test_doubles twister scenario (CONFIG_ALP_SDK_TESTING=y) -- see
 * testcase.yaml. Drives the double through the PUBLIC alp/peripheral.h
 * API plus the alp/testing injection surface; never touches
 * i2c_ops.h / testing_drv.c internals directly.
 *
 * Built alongside src/behavior_gpio.c, src/behavior_uart.c and
 * src/behavior_spi.c in this scenario's app image (this scenario's
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
#include <alp/testing/i2c.h>

/* Local copy of main.c's / behavior_gpio.c's enum-membership helper --
 * every behavior_*.c is static to its own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static alp_i2c_t *open_bus(uint32_t bus_id)
{
	alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(bus_id);
	return alp_i2c_open(&cfg);
}

static void i2c_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
}

/* After-each teardown, mirroring behavior_uart.c's dispatcher
 * pool-health check: alp_testing_reset_all() wipes the testing
 * double's own state but cannot reach the DISPATCHER's private static
 * handle pool (CONFIG_ALP_SDK_MAX_I2C_HANDLES slots,
 * src/i2c_dispatch.c). A test that leaks a handle would otherwise
 * silently shrink the pool for every later test until it quietly runs
 * out, surfacing as a confusing ALP_ERR_NOMEM far from the actual
 * leak. Round-tripping a fresh handle here fails loudly instead. */
static void i2c_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_i2c_t *h = open_bus(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_i2c_open(0) returned NULL right after "
	                 "this test -- a prior test in this file leaked a handle out of the "
	                 "dispatcher's fixed-size pool");
	alp_i2c_close(h);
}

ZTEST_SUITE(alp_testing_i2c_behavior, NULL, NULL, i2c_behavior_before, i2c_behavior_after, NULL);

/* Setup-fixture-shaped assertion (mirrors behavior_gpio.c's / behavior_uart.c's):
 * a mis-selection must fail LOUDLY, not silently exercise the wrong
 * backend for every other case below. */
ZTEST(alp_testing_i2c_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("i2c", ALP_SOC_REF_STR);

	zassert_not_null(be, "i2c class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "i2c backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_I2C "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "i2c backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

/* Backend-selection assertion beyond the fixture's own check --
 * exercised again explicitly per the task's deliverable list. */
ZTEST(alp_testing_i2c_behavior, test_backend_vendor_is_alp_testing)
{
	const alp_backend_t *be = alp_backend_select("i2c", ALP_SOC_REF_STR);

	zassert_not_null(be, "i2c class has no registered backend at all");
	zassert_equal(strcmp(be->vendor, "alp_testing"), 0, "vendor must be alp_testing");
}

ZTEST(alp_testing_i2c_behavior, test_write_captures_last_write)
{
	const uint32_t bus_id = 20;
	const uint8_t  addr   = 0x50;
	const uint8_t  msg[]  = { 0x01, 0x02, 0x03 };

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");

	zassert_equal(alp_i2c_write(h, addr, msg, sizeof(msg)), ALP_OK, "write() failed");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(alp_testing_i2c_last_write(bus_id, addr, out, sizeof(out), &got),
	              ALP_OK,
	              "last_write failed");
	zassert_equal(got, sizeof(msg), "last_write returned %zu, expected %zu", got, sizeof(msg));
	zassert_mem_equal(out, msg, sizeof(msg), "last_write did not observe the written bytes");

	alp_i2c_close(h);
}

ZTEST(alp_testing_i2c_behavior, test_read_returns_canned_response)
{
	const uint32_t bus_id = 21;
	const uint8_t  addr   = 0x51;
	const uint8_t  rsp[]  = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_equal(alp_testing_i2c_target_respond(bus_id, addr, rsp, sizeof(rsp)),
	              ALP_OK,
	              "target_respond failed");

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");

	uint8_t buf[sizeof(rsp)] = { 0 };
	zassert_equal(alp_i2c_read(h, addr, buf, sizeof(buf)), ALP_OK, "read() failed");
	zassert_mem_equal(buf, rsp, sizeof(rsp), "read() did not observe the canned response");

	/* Canned response is a persistent snapshot, not a drained queue:
	 * a second read observes the same data. */
	uint8_t buf2[sizeof(rsp)] = { 0 };
	zassert_equal(alp_i2c_read(h, addr, buf2, sizeof(buf2)), ALP_OK, "second read() failed");
	zassert_mem_equal(buf2, rsp, sizeof(rsp), "second read() must observe the same canned data");

	alp_i2c_close(h);
}

ZTEST(alp_testing_i2c_behavior, test_write_read_round_trip)
{
	const uint32_t bus_id = 22;
	const uint8_t  addr   = 0x52;
	const uint8_t  reg[]  = { 0x00 };
	const uint8_t  rsp[]  = { 0x42, 0x43 };

	zassert_equal(alp_testing_i2c_target_respond(bus_id, addr, rsp, sizeof(rsp)),
	              ALP_OK,
	              "target_respond failed");

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");

	uint8_t rdata[sizeof(rsp)] = { 0 };
	zassert_equal(alp_i2c_write_read(h, addr, reg, sizeof(reg), rdata, sizeof(rdata)),
	              ALP_OK,
	              "write_read() failed");
	zassert_mem_equal(rdata, rsp, sizeof(rsp), "write_read() did not return the canned response");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(alp_testing_i2c_last_write(bus_id, addr, out, sizeof(out), &got),
	              ALP_OK,
	              "last_write failed");
	zassert_equal(got, sizeof(reg), "write_read()'s write phase must be captured too");
	zassert_mem_equal(
	    out, reg, sizeof(reg), "last_write did not observe write_read()'s write phase");

	alp_i2c_close(h);
}

/* NACK: documented status (ALP_ERR_IO), zero bytes captured. */
ZTEST(alp_testing_i2c_behavior, test_nack_returns_documented_status)
{
	const uint32_t bus_id = 23;
	const uint8_t  addr   = 0x53;
	const uint8_t  msg[]  = { 0x11, 0x22 };

	zassert_equal(alp_testing_i2c_fail_next(bus_id, addr, ALP_TESTING_FAULT_NACK, 0),
	              ALP_OK,
	              "fail_next failed");

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");

	alp_status_t s = alp_i2c_write(h, addr, msg, sizeof(msg));
	zassert_equal(s, ALP_ERR_IO, "write() must surface the documented NACK status (ALP_ERR_IO)");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	uint8_t out[8] = { 0 };
	size_t  got    = 1; /* poison to prove last_write zeroes it */
	zassert_equal(alp_testing_i2c_last_write(bus_id, addr, out, sizeof(out), &got),
	              ALP_OK,
	              "last_write failed");
	zassert_equal(got, 0, "a NACKed write must capture zero bytes");

	/* One-shot: the fault disarms itself, so the next write succeeds. */
	zassert_equal(alp_i2c_write(h, addr, msg, sizeof(msg)),
	              ALP_OK,
	              "fail_next must be one-shot -- the following write must succeed");

	alp_i2c_close(h);
}

/* Short transfer: only `short_len` bytes actually captured, then the
 * documented bus-error status. */
ZTEST(alp_testing_i2c_behavior, test_short_transfer_partial)
{
	const uint32_t bus_id    = 24;
	const uint8_t  addr      = 0x54;
	const uint8_t  msg[]     = { 0xDE, 0xAD, 0xBE, 0xEF };
	const size_t   short_len = 2;

	zassert_equal(alp_testing_i2c_fail_next(bus_id, addr, ALP_TESTING_FAULT_SHORT, short_len),
	              ALP_OK,
	              "fail_next failed");

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");

	zassert_equal(alp_i2c_write(h, addr, msg, sizeof(msg)),
	              ALP_ERR_IO,
	              "short write must surface ALP_ERR_IO");

	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(alp_testing_i2c_last_write(bus_id, addr, out, sizeof(out), &got),
	              ALP_OK,
	              "last_write failed");
	zassert_equal(got, short_len, "a short write must capture exactly short_len bytes");
	zassert_mem_equal(out, msg, short_len, "short write captured the wrong bytes");

	alp_i2c_close(h);
}

/* Reset-frees-side-state regression: prime a canned response, capture
 * a write, arm a fault -- then reset_all() and prove the bus/address is
 * fully reusable with none of that state carried over. */
ZTEST(alp_testing_i2c_behavior, test_reset_all_frees_side_state)
{
	const uint32_t bus_id = 25;
	const uint8_t  addr   = 0x55;
	const uint8_t  rsp[]  = { 0x9A };
	const uint8_t  wr[]   = { 0x01 };

	zassert_equal(alp_testing_i2c_target_respond(bus_id, addr, rsp, sizeof(rsp)),
	              ALP_OK,
	              "target_respond failed");
	zassert_equal(alp_testing_i2c_fail_next(bus_id, addr, ALP_TESTING_FAULT_NACK, 0),
	              ALP_OK,
	              "fail_next failed");

	alp_i2c_t *h = open_bus(bus_id);
	zassert_not_null(h, "i2c test double must open ANY instance");
	/* Consume the armed NACK first so it doesn't leak into the
	 * reset_all() assertions below. */
	zassert_equal(alp_i2c_write(h, addr, wr, sizeof(wr)), ALP_ERR_IO, "expected the armed NACK");
	alp_i2c_close(h);

	alp_testing_reset_all();

	/* last_write must report "never touched" again (ALP_ERR_INVAL),
	 * not the pre-reset capture -- checked BEFORE anything re-touches
	 * (bus_id, addr), since last_write itself is a pure lookup that
	 * must not implicitly re-create the slot. */
	uint8_t out[8] = { 0 };
	size_t  got    = 0;
	zassert_equal(alp_testing_i2c_last_write(bus_id, addr, out, sizeof(out), &got),
	              ALP_ERR_INVAL,
	              "reset_all() must have cleared last_write's side-state for this address");

	h = open_bus(bus_id);
	zassert_not_null(h, "bus must be reusable after reset_all()");

	/* Canned response must be gone: a fresh read returns zeros, not
	 * the pre-reset rsp[]. */
	uint8_t buf[sizeof(rsp)] = { 0xFF };
	zassert_equal(alp_i2c_read(h, addr, buf, sizeof(buf)), ALP_OK, "read() failed");
	zassert_equal(buf[0], 0, "reset_all() must have cleared the canned response");

	alp_i2c_close(h);
}

/* #610 review (test-completeness): the I2C test double models the
 * OTHER device on the bus, not this MCU's own target (slave) mode --
 * <alp/peripheral.h>'s alp_i2c_target_open() doc and this double's own
 * header comment both say target_open/target_close are left NULL so
 * the dispatcher's own ALP_ERR_NOSUPPORT degrade path fires, but no
 * test asserted it. Pin it here. */
static void stub_on_write(uint8_t byte, void *user)
{
	ARG_UNUSED(byte);
	ARG_UNUSED(user);
}

static alp_status_t stub_on_read(uint8_t *byte, void *user)
{
	ARG_UNUSED(user);
	*byte = 0;
	return ALP_OK;
}

ZTEST(alp_testing_i2c_behavior, test_target_open_is_nosupport_on_this_double)
{
	const uint32_t bus_id = 26;

	alp_i2c_target_config_t cfg = ALP_I2C_TARGET_CONFIG_DEFAULT(bus_id);
	cfg.own_addr_7bit           = 0x50;
	cfg.on_write                = stub_on_write;
	cfg.on_read                 = stub_on_read;

	alp_i2c_target_t *tgt = alp_i2c_target_open(&cfg);
	zassert_is_null(tgt,
	                "target_open() must fail on this double -- it models the OTHER device, "
	                "not this MCU's target mode");
	zassert_equal(alp_last_error(),
	              ALP_ERR_NOSUPPORT,
	              "target_open() failure must surface ALP_ERR_NOSUPPORT (target_open left NULL "
	              "in this double's ops table)");
}
