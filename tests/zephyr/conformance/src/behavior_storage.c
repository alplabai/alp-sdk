/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing Storage virtual backend
 * (epic #610 §2 -- the last of the fan-out). Compiled only for the
 * alp_sdk.conformance.test_doubles twister scenario
 * (CONFIG_ALP_SDK_TESTING=y) -- see testcase.yaml. Drives the double
 * through the PUBLIC alp/storage.h API plus the alp/testing injection
 * surface; never touches storage_ops.h / testing_drv.c internals
 * directly.
 *
 * Built alongside src/behavior_gpio.c, src/behavior_uart.c,
 * src/behavior_i2c.c, src/behavior_spi.c, src/behavior_adc.c and
 * src/behavior_can.c in this scenario's app image (this scenario's
 * CMakeLists swaps all of these in for src/main.c) -- see the top of
 * behavior_gpio.c for why main.c's per-class expectations are
 * incompatible with a priority-255 "open ANY instance" test double and
 * must never share a binary with it.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/soc_caps.h>
#include <alp/storage.h>
#include <alp/testing/common.h>
#include <alp/testing/storage.h>

/* Local copy of main.c's / behavior_gpio.c's enum-membership helper --
 * every behavior_*.c is static to its own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

/* <alp/storage.h>'s ALP_STORAGE_CONFIG_DEFAULT(id) sets `.kind`, not an
 * instance id (unlike the bus/channel-keyed classes' _CONFIG_DEFAULT
 * macros) -- "there is no universally-safe kind to default to", per
 * its own doc comment -- so the storage_id this double keys off
 * (alp_storage_config_t.instance_id) is set explicitly here instead. */
static alp_storage_t *open_storage(uint32_t storage_id)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_INTERNAL_FLASH);
	cfg.instance_id          = storage_id;
	return alp_storage_open(&cfg);
}

static void storage_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
}

/* After-each teardown (mirrors behavior_can.c's / behavior_i2c.c's
 * dispatcher pool-health check): alp_testing_reset_all() wipes the
 * testing double's own state but cannot reach the DISPATCHER's
 * private static handle pool (CONFIG_ALP_SDK_MAX_STORAGE_HANDLES
 * slots, src/storage_dispatch.c). A test that leaks a handle would
 * otherwise silently shrink the pool for every later test until it
 * quietly runs out, surfacing as a confusing ALP_ERR_NOMEM far from
 * the actual leak. Round-tripping a fresh handle here fails loudly
 * instead. */
static void storage_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_storage_t *h = open_storage(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_storage_open(instance_id=0) returned NULL "
	                 "right after this test -- a prior test in this file leaked a handle out of "
	                 "the dispatcher's fixed-size pool");
	alp_storage_close(h);
}

ZTEST_SUITE(alp_testing_storage_behavior,
            NULL,
            NULL,
            storage_behavior_before,
            storage_behavior_after,
            NULL);

/* Setup-fixture-shaped assertion (mirrors every other behavior_*.c's):
 * a mis-selection must fail LOUDLY, not silently exercise the wrong
 * backend for every other case below. */
ZTEST(alp_testing_storage_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("storage", ALP_SOC_REF_STR);

	zassert_not_null(be, "storage class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "storage backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_STORAGE "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "storage backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

/* Backend-selection assertion beyond the fixture's own check --
 * exercised again explicitly per the task's deliverable list. */
ZTEST(alp_testing_storage_behavior, test_backend_vendor_is_alp_testing)
{
	const alp_backend_t *be = alp_backend_select("storage", ALP_SOC_REF_STR);

	zassert_not_null(be, "storage class has no registered backend at all");
	zassert_equal(strcmp(be->vendor, "alp_testing"), 0, "vendor must be alp_testing");
}

ZTEST(alp_testing_storage_behavior, test_write_read_round_trip)
{
	const uint32_t storage_id = 40;
	const uint8_t  payload[]  = { 0xDE, 0xAD, 0xBE, 0xEF };

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)), ALP_OK, "write() failed");

	uint8_t buf[sizeof(payload)] = { 0 };
	zassert_equal(alp_storage_read(h, 0, buf, sizeof(buf)), ALP_OK, "read() failed");
	zassert_mem_equal(buf, payload, sizeof(payload), "read() did not observe the written bytes");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_get_info_reports_injected_capacity)
{
	const uint32_t storage_id = 41;
	const uint64_t capacity   = 2048;

	zassert_equal(
	    alp_testing_storage_set_capacity(storage_id, capacity), ALP_OK, "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	alp_storage_info_t info = { 0 };
	zassert_equal(alp_storage_get_info(h, &info), ALP_OK, "get_info() failed");
	zassert_equal(info.total_bytes,
	              capacity,
	              "get_info() must report the capacity injected via set_capacity()");
	zassert_equal(info.block_size, 1u, "this double documents block_size == 1 (byte-addressable)");
	zassert_true(info.erase_size > 0u, "erase_size must be a real, non-zero granule");

	alp_storage_close(h);
}

/* Documented ALP_ERR_OUT_OF_RANGE case: offset + len past device end. */
ZTEST(alp_testing_storage_behavior, test_write_past_capacity_returns_documented_status)
{
	const uint32_t storage_id = 42;
	const uint8_t  payload[]  = { 1, 2, 3, 4 };

	zassert_equal(alp_testing_storage_set_capacity(storage_id, 4), ALP_OK, "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	/* offset 2 + len 4 == 6, past the 4-byte capacity. */
	alp_status_t s = alp_storage_write(h, 2, payload, sizeof(payload));
	zassert_equal(s,
	              ALP_ERR_OUT_OF_RANGE,
	              "write() past capacity must surface the documented ALP_ERR_OUT_OF_RANGE");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Same rule applies to read(). */
	uint8_t buf[sizeof(payload)] = { 0 };
	zassert_equal(alp_storage_read(h, 2, buf, sizeof(buf)),
	              ALP_ERR_OUT_OF_RANGE,
	              "read() past capacity must surface ALP_ERR_OUT_OF_RANGE too");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_erase_region_reads_erased)
{
	const uint32_t storage_id = 43;

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	alp_storage_info_t info = { 0 };
	zassert_equal(alp_storage_get_info(h, &info), ALP_OK, "get_info() failed");

	uint8_t *pattern = k_malloc(info.erase_size);
	zassert_not_null(pattern, "test allocation failed");
	memset(pattern, 0x5A, info.erase_size);

	zassert_equal(
	    alp_storage_write(h, 0, pattern, info.erase_size), ALP_OK, "priming write() failed");
	zassert_equal(alp_storage_erase(h, 0, info.erase_size), ALP_OK, "erase() failed");

	uint8_t *readback = k_malloc(info.erase_size);
	zassert_not_null(readback, "test allocation failed");
	zassert_equal(alp_storage_read(h, 0, readback, info.erase_size), ALP_OK, "read() failed");

	for (uint32_t i = 0; i < info.erase_size; ++i) {
		zassert_equal(readback[i],
		              0xFFu,
		              "erased byte %u must read back as 0xFF (NOR-erased-state convention), got "
		              "0x%02x",
		              i,
		              readback[i]);
	}

	k_free(pattern);
	k_free(readback);
	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_corruption_then_overwrite_clears_it)
{
	const uint32_t storage_id = 44;
	const uint8_t  payload[]  = { 0x11, 0x22, 0x33, 0x44 };

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)), ALP_OK, "write() failed");
	zassert_equal(alp_testing_storage_inject_corruption(storage_id, 1, 2),
	              ALP_OK,
	              "inject_corruption failed");

	uint8_t      buf[sizeof(payload)] = { 0 };
	alp_status_t s                    = alp_storage_read(h, 0, buf, sizeof(buf));
	zassert_equal(s,
	              ALP_ERR_IO,
	              "read() overlapping a corrupt region must surface the documented ALP_ERR_IO");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Overwriting the corrupt bytes clears the mark -- "until
	 * overwritten", per <alp/testing/storage.h>. */
	const uint8_t fresh[] = { 0xAA, 0xBB };
	zassert_equal(alp_storage_write(h, 1, fresh, sizeof(fresh)), ALP_OK, "overwrite failed");

	uint8_t buf2[sizeof(payload)] = { 0 };
	zassert_equal(alp_storage_read(h, 0, buf2, sizeof(buf2)),
	              ALP_OK,
	              "read() must succeed once the corrupt bytes have been overwritten");
	zassert_equal(buf2[0], payload[0], "byte 0 (untouched) must be unchanged");
	zassert_mem_equal(&buf2[1], fresh, sizeof(fresh), "bytes 1..2 must reflect the overwrite");
	zassert_equal(buf2[3], payload[3], "byte 3 (untouched) must be unchanged");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_fail_next_read)
{
	const uint32_t storage_id = 45;

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_READ, ALP_ERR_TIMEOUT),
	    ALP_OK,
	    "fail_next failed");

	uint8_t buf[4] = { 0 };
	zassert_equal(alp_storage_read(h, 0, buf, sizeof(buf)),
	              ALP_ERR_TIMEOUT,
	              "read() must surface the armed status");

	/* One-shot: disarms itself. */
	zassert_equal(
	    alp_storage_read(h, 0, buf, sizeof(buf)), ALP_OK, "fail_next must be one-shot for read()");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_fail_next_write)
{
	const uint32_t storage_id = 46;
	const uint8_t  payload[]  = { 1, 2, 3, 4 };

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_WRITE, ALP_ERR_BUSY),
	    ALP_OK,
	    "fail_next failed");

	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)),
	              ALP_ERR_BUSY,
	              "write() must surface the armed status");
	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)),
	              ALP_OK,
	              "fail_next must be one-shot for write()");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_fail_next_erase)
{
	const uint32_t storage_id = 47;

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	alp_storage_info_t info = { 0 };
	zassert_equal(alp_storage_get_info(h, &info), ALP_OK, "get_info() failed");

	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_ERASE, ALP_ERR_IO),
	    ALP_OK,
	    "fail_next failed");

	zassert_equal(alp_storage_erase(h, 0, info.erase_size),
	              ALP_ERR_IO,
	              "erase() must surface the armed status");
	zassert_equal(
	    alp_storage_erase(h, 0, info.erase_size), ALP_OK, "fail_next must be one-shot for erase()");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_fail_next_sync)
{
	const uint32_t storage_id = 48;

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_SYNC, ALP_ERR_IO),
	    ALP_OK,
	    "fail_next failed");

	zassert_equal(alp_storage_sync(h), ALP_ERR_IO, "sync() must surface the armed status");
	zassert_equal(alp_storage_sync(h), ALP_OK, "fail_next must be one-shot for sync()");

	alp_storage_close(h);
}

/* Compile-time constants (not `const` locals) so the buffers below are
 * fixed-size, not VLAs -- CONFIG_COMPILER_WARNINGS_AS_ERRORS=y treats
 * -Wvla as a hard error under the strict-warnings profile. */
#define POWER_LOSS_TEST_PAYLOAD_LEN   8u
#define POWER_LOSS_TEST_BYTES_WRITTEN 3u /* N: persisted before the simulated power loss */

/* THE STORAGE-SPECIFIC MUST (issue #610 §2): a mid-write power loss
 * leaves a provably TORN write -- the persisted prefix matches the NEW
 * payload, the untouched tail retains the PRIOR bytes (not zeroed, not
 * overwritten), and alp_storage_write() itself surfaces the documented
 * power-loss/I/O status.
 *
 * A region that was never written before reads back as fresh-zero
 * regardless of whether an implementation actually leaves prior bytes
 * untouched OR erroneously zeroes the torn tail -- either way the
 * assertion "torn tail == 0" passes, so it does not distinguish the two.
 * To make the "prior bytes untouched" property load-bearing, this test
 * primes the region with a KNOWN non-zero pattern (0xAA) before arming
 * the cut, writes a DIFFERENT payload (0x55) over it, and then asserts
 * the torn tail equals the OLD 0xAA pattern -- not zero, and not the new
 * payload's tail. */
ZTEST(alp_testing_storage_behavior, test_power_loss_leaves_torn_write)
{
	const uint32_t storage_id = 49;
	const size_t   len        = POWER_LOSS_TEST_PAYLOAD_LEN;
	const size_t   n          = POWER_LOSS_TEST_BYTES_WRITTEN;
	const size_t   torn_len   = len - n;

	uint8_t old_pattern[POWER_LOSS_TEST_PAYLOAD_LEN];
	uint8_t new_payload[POWER_LOSS_TEST_PAYLOAD_LEN];
	memset(old_pattern, 0xAA, sizeof(old_pattern));
	memset(new_payload, 0x55, sizeof(new_payload));

	zassert_equal(alp_testing_storage_set_capacity(storage_id, len), ALP_OK, "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	/* Prime with a KNOWN non-zero pattern -- see the comment above. */
	zassert_equal(alp_storage_write(h, 0, old_pattern, sizeof(old_pattern)),
	              ALP_OK,
	              "priming write() failed");

	zassert_equal(alp_testing_storage_inject_power_loss_after(storage_id, n),
	              ALP_OK,
	              "inject_power_loss_after failed");

	/* M (len == 8) > N (n == 3): write a DIFFERENT payload over the
	 * primed region; the power-loss cut tears it after the first N
	 * bytes. */
	alp_status_t s = alp_storage_write(h, 0, new_payload, sizeof(new_payload));
	zassert_equal(
	    s, ALP_ERR_IO, "a power-loss-interrupted write must surface the documented ALP_ERR_IO");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Torn shape, proven via read_back() -- bypasses any read fault /
	 * corruption logic and reports exactly what is persisted. */
	uint8_t persisted_prefix[POWER_LOSS_TEST_BYTES_WRITTEN];
	zassert_equal(
	    alp_testing_storage_read_back(storage_id, 0, persisted_prefix, sizeof(persisted_prefix)),
	    ALP_OK,
	    "read_back failed");
	zassert_mem_equal(persisted_prefix,
	                  new_payload,
	                  sizeof(persisted_prefix),
	                  "the first bytes_written bytes of the NEW payload MUST be persisted");

	uint8_t torn_tail[POWER_LOSS_TEST_PAYLOAD_LEN] = { 0 };
	zassert_equal(alp_testing_storage_read_back(storage_id, n, torn_tail, torn_len),
	              ALP_OK,
	              "read_back failed");

	/* The load-bearing assertion: the torn tail must equal the OLD
	 * pattern that was already there -- NOT zero, and NOT the new
	 * payload's tail. An implementation that erroneously zeroed the
	 * unpersisted tail (instead of leaving prior bytes untouched) would
	 * pass a zero-only check identically; comparing against a known
	 * non-zero prior value is what actually proves "untouched". */
	zassert_mem_equal(torn_tail,
	                  &old_pattern[n],
	                  torn_len,
	                  "the torn tail must retain the PRIOR bytes untouched, not be zeroed or "
	                  "otherwise overwritten");
	zassert_true(memcmp(torn_tail, &new_payload[n], torn_len) != 0,
	             "the torn tail must NOT match the new payload's tail -- that would mean the "
	             "write was not actually torn");

	/* One-shot: the NEXT write is unaffected by the power-loss cut. */
	zassert_equal(alp_storage_write(h, 0, new_payload, sizeof(new_payload)),
	              ALP_OK,
	              "inject_power_loss_after must be one-shot");

	uint8_t full_readback[POWER_LOSS_TEST_PAYLOAD_LEN] = { 0 };
	zassert_equal(
	    alp_testing_storage_read_back(storage_id, 0, full_readback, sizeof(full_readback)),
	    ALP_OK,
	    "read_back failed");
	zassert_mem_equal(full_readback,
	                  new_payload,
	                  sizeof(new_payload),
	                  "the follow-up write must persist the WHOLE payload");

	alp_storage_close(h);
}

/* Fix #2 (issue #610 §2 review follow-up): the torn-write test above
 * only exercises offset 0, where "tear at offset+bytes_written" and
 * "tear at bare bytes_written" are indistinguishable. Repeat at a
 * NON-ZERO offset to prove the double uses offset+bytes_written, not a
 * bare bytes_written, as the tear point. */
ZTEST(alp_testing_storage_behavior, test_power_loss_leaves_torn_write_at_nonzero_offset)
{
	const uint32_t storage_id = 53;
	const uint64_t offset     = 5u;
	const size_t   len        = POWER_LOSS_TEST_PAYLOAD_LEN;
	const size_t   n          = POWER_LOSS_TEST_BYTES_WRITTEN;
	const size_t   torn_len   = len - n;

	uint8_t old_pattern[POWER_LOSS_TEST_PAYLOAD_LEN];
	uint8_t new_payload[POWER_LOSS_TEST_PAYLOAD_LEN];
	memset(old_pattern, 0xAA, sizeof(old_pattern));
	memset(new_payload, 0x55, sizeof(new_payload));

	zassert_equal(
	    alp_testing_storage_set_capacity(storage_id, offset + len), ALP_OK, "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(alp_storage_write(h, offset, old_pattern, sizeof(old_pattern)),
	              ALP_OK,
	              "priming write() failed");

	zassert_equal(alp_testing_storage_inject_power_loss_after(storage_id, n),
	              ALP_OK,
	              "inject_power_loss_after failed");

	alp_status_t s = alp_storage_write(h, offset, new_payload, sizeof(new_payload));
	zassert_equal(s,
	              ALP_ERR_IO,
	              "a power-loss-interrupted write at a non-zero offset must still surface "
	              "ALP_ERR_IO");

	uint8_t persisted_prefix[POWER_LOSS_TEST_BYTES_WRITTEN];
	zassert_equal(alp_testing_storage_read_back(
	                  storage_id, offset, persisted_prefix, sizeof(persisted_prefix)),
	              ALP_OK,
	              "read_back failed");
	zassert_mem_equal(persisted_prefix,
	                  new_payload,
	                  sizeof(persisted_prefix),
	                  "the persisted prefix must be at [offset, offset+bytes_written) -- the tear "
	                  "point must be offset+bytes_written, not bare bytes_written");

	uint8_t torn_tail[POWER_LOSS_TEST_PAYLOAD_LEN] = { 0 };
	zassert_equal(alp_testing_storage_read_back(storage_id, offset + n, torn_tail, torn_len),
	              ALP_OK,
	              "read_back failed");
	zassert_mem_equal(torn_tail,
	                  &old_pattern[n],
	                  torn_len,
	                  "the torn tail at [offset+bytes_written, offset+len) must retain the PRIOR "
	                  "bytes untouched");

	alp_storage_close(h);
}

/* Fix #3a (issue #610 §2 review follow-up): double-arm ordering. An
 * armed fail_next(OP_WRITE, ...) fires BEFORE an armed power-loss cut
 * (see the @note on alp_testing_storage_inject_power_loss_after() in
 * <alp/testing/storage.h> and t_write()'s check order in
 * testing_drv.c) -- the fail_next branch returns before ever touching
 * the backing buffer, so nothing persists, and the power-loss cut
 * (untouched by the failed write) stays armed for the NEXT write. */
ZTEST(alp_testing_storage_behavior, test_fail_next_write_fires_before_armed_power_loss)
{
	const uint32_t storage_id                           = 54;
	const uint8_t  payload[POWER_LOSS_TEST_PAYLOAD_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const size_t   n                                    = POWER_LOSS_TEST_BYTES_WRITTEN;

	zassert_equal(alp_testing_storage_set_capacity(storage_id, sizeof(payload)),
	              ALP_OK,
	              "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_WRITE, ALP_ERR_BUSY),
	    ALP_OK,
	    "fail_next failed");
	zassert_equal(alp_testing_storage_inject_power_loss_after(storage_id, n),
	              ALP_OK,
	              "inject_power_loss_after failed");

	alp_status_t s1 = alp_storage_write(h, 0, payload, sizeof(payload));
	zassert_equal(
	    s1, ALP_ERR_BUSY, "an armed fail_next(OP_WRITE) must fire before an armed power-loss cut");

	uint8_t rb[POWER_LOSS_TEST_PAYLOAD_LEN] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu,
		                                        0xFFu, 0xFFu, 0xFFu, 0xFFu };
	zassert_equal(
	    alp_testing_storage_read_back(storage_id, 0, rb, sizeof(rb)), ALP_OK, "read_back failed");
	for (size_t i = 0; i < sizeof(rb); ++i) {
		zassert_equal(
		    rb[i],
		    0,
		    "the fail_next()-rejected write must not have persisted any bytes, got 0x%02x at %zu",
		    rb[i],
		    i);
	}

	/* fail_next was one-shot and is now disarmed; the power-loss cut
	 * armed earlier was untouched by the failed write and fires on THIS
	 * write instead. */
	alp_status_t s2 = alp_storage_write(h, 0, payload, sizeof(payload));
	zassert_equal(s2,
	              ALP_ERR_IO,
	              "the power-loss cut armed before the failed write must still be armed for the "
	              "next write");

	uint8_t persisted_prefix[POWER_LOSS_TEST_BYTES_WRITTEN];
	zassert_equal(
	    alp_testing_storage_read_back(storage_id, 0, persisted_prefix, sizeof(persisted_prefix)),
	    ALP_OK,
	    "read_back failed");
	zassert_mem_equal(persisted_prefix,
	                  payload,
	                  sizeof(persisted_prefix),
	                  "the deferred power-loss cut must tear this second write");

	alp_storage_close(h);
}

/* Fix #3b (issue #610 §2 review follow-up): bytes_written >= len clamps
 * to len -- the WHOLE payload persists (see the @param doc on
 * alp_testing_storage_inject_power_loss_after() in
 * <alp/testing/storage.h>) -- but the write() call still returns
 * ALP_ERR_IO, modelling a power loss that occurred after the last byte
 * was latched but before write() could report success. */
ZTEST(alp_testing_storage_behavior, test_power_loss_bytes_written_ge_len_clamps_but_still_errors)
{
	const uint32_t storage_id = 55;
	const uint8_t  payload[]  = { 0x10, 0x20, 0x30, 0x40 };

	zassert_equal(alp_testing_storage_set_capacity(storage_id, sizeof(payload)),
	              ALP_OK,
	              "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	/* bytes_written (len + 100) > len (4). */
	zassert_equal(alp_testing_storage_inject_power_loss_after(storage_id, sizeof(payload) + 100u),
	              ALP_OK,
	              "inject_power_loss_after failed");

	alp_status_t s = alp_storage_write(h, 0, payload, sizeof(payload));
	zassert_equal(s,
	              ALP_ERR_IO,
	              "bytes_written >= len must still surface ALP_ERR_IO -- write() itself never got "
	              "to report success");

	uint8_t rb[sizeof(payload)] = { 0 };
	zassert_equal(
	    alp_testing_storage_read_back(storage_id, 0, rb, sizeof(rb)), ALP_OK, "read_back failed");
	zassert_mem_equal(rb,
	                  payload,
	                  sizeof(rb),
	                  "bytes_written >= len must clamp to len -- the WHOLE payload is persisted "
	                  "despite the ALP_ERR_IO return");

	alp_storage_close(h);
}

ZTEST(alp_testing_storage_behavior, test_persistence_across_close_reopen)
{
	const uint32_t storage_id = 50;
	const uint8_t  payload[]  = { 0xAB, 0xCD, 0xEF };

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");
	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)), ALP_OK, "write() failed");
	alp_storage_close(h);

	/* Non-volatile model: a fresh open() on the same storage_id must
	 * observe what was there before -- close() does not wipe it. */
	alp_storage_t *h2 = open_storage(storage_id);
	zassert_not_null(h2, "re-open on the same storage_id must succeed");

	uint8_t buf[sizeof(payload)] = { 0 };
	zassert_equal(alp_storage_read(h2, 0, buf, sizeof(buf)), ALP_OK, "read() after re-open failed");
	zassert_mem_equal(
	    buf, payload, sizeof(payload), "data written before close() must survive the re-open");

	alp_storage_close(h2);
}

/* Reset-frees-side-state regression: prime capacity, corruption, an
 * armed fault, an armed power-loss cut AND written data on one
 * storage_id -- then reset_all() and prove the id is fully reusable
 * with none of that state carried over. */
ZTEST(alp_testing_storage_behavior, test_reset_all_frees_side_state)
{
	const uint32_t storage_id = 51;
	const uint8_t  payload[]  = { 0x9A, 0x9B, 0x9C, 0x9D };

	zassert_equal(alp_testing_storage_set_capacity(storage_id, 8), ALP_OK, "set_capacity failed");

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");
	zassert_equal(alp_storage_write(h, 0, payload, sizeof(payload)), ALP_OK, "write() failed");

	zassert_equal(alp_testing_storage_inject_corruption(storage_id, 0, 4),
	              ALP_OK,
	              "inject_corruption failed");
	zassert_equal(
	    alp_testing_storage_fail_next(storage_id, ALP_TESTING_STORAGE_OP_READ, ALP_ERR_IO),
	    ALP_OK,
	    "fail_next failed");
	zassert_equal(alp_testing_storage_inject_power_loss_after(storage_id, 1),
	              ALP_OK,
	              "inject_power_loss_after failed");

	alp_storage_close(h);
	alp_testing_reset_all();

	/* read_back on a not-yet-re-touched storage_id reports "never
	 * touched" (ALP_ERR_INVAL), checked BEFORE anything re-creates the
	 * slot -- mirrors the I2C double's last_write regression. */
	uint8_t rb[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	zassert_equal(alp_testing_storage_read_back(storage_id, 0, rb, sizeof(rb)),
	              ALP_ERR_INVAL,
	              "reset_all() must have cleared this storage_id's slot binding");

	alp_storage_t *h2 = open_storage(storage_id);
	zassert_not_null(h2, "storage_id must be reusable after reset_all()");

	/* No armed read fault, no corruption, fresh zero-filled data --
	 * every side-effect from before the reset must be gone. */
	uint8_t buf[sizeof(payload)] = { 0xFF, 0xFF, 0xFF, 0xFF };
	zassert_equal(alp_storage_read(h2, 0, buf, sizeof(buf)),
	              ALP_OK,
	              "reset_all() must have cleared the armed read fault + corruption mark");
	for (size_t i = 0; i < sizeof(buf); ++i) {
		zassert_equal(buf[i], 0, "reset_all() must have cleared the previously-written data");
	}

	/* No pending power-loss cut either: a full-length write persists
	 * in full. */
	zassert_equal(alp_storage_write(h2, 0, payload, sizeof(payload)),
	              ALP_OK,
	              "reset_all() must have cleared the armed power-loss cut");
	uint8_t buf2[sizeof(payload)] = { 0 };
	zassert_equal(alp_storage_read(h2, 0, buf2, sizeof(buf2)), ALP_OK, "read() failed");
	zassert_mem_equal(buf2, payload, sizeof(payload), "post-reset write must persist in full");

	alp_storage_close(h2);
}

/* configure_inline_aes() is not modelled by this double -- always
 * ALP_ERR_NOSUPPORT, matching sw_fallback.c and every
 * non-vendor-extension backend (see the @note on
 * <alp/testing/storage.h>). Pin it here. */
ZTEST(alp_testing_storage_behavior, test_configure_inline_aes_is_nosupport_on_this_double)
{
	const uint32_t storage_id = 52;
	const uint8_t  key[16]    = { 0 };
	const uint8_t  iv[16]     = { 0 };

	alp_storage_t *h = open_storage(storage_id);
	zassert_not_null(h, "storage test double must open ANY instance");

	alp_storage_aes_config_t cfg = {
		.mode      = ALP_STORAGE_AES_CTR,
		.key       = key,
		.key_bytes = sizeof(key),
		.iv        = iv,
		.iv_bytes  = sizeof(iv),
		.reserved  = 0,
	};

	zassert_equal(alp_storage_configure_inline_aes(h, &cfg),
	              ALP_ERR_NOSUPPORT,
	              "configure_inline_aes() must surface ALP_ERR_NOSUPPORT on this double");

	alp_storage_close(h);
}
