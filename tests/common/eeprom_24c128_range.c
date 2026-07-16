/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #738: eeprom_24c128_read()/eeprom_24c128_write()
 * validated their offset/len range with the overflow-prone
 * `(uint32_t)offset + len > EEPROM_24C128_BYTES` form.  Because `len` is a
 * `size_t`, a sufficiently large `len` (e.g. near SIZE_MAX) wraps the sum
 * before the comparison ever runs, so the range check silently passes and
 * the call falls through to the I2C transfer / page-write memcpy with an
 * out-of-range offset/len pair -- a real out-of-bounds device access on the
 * 24C128 (this part is populated on the AEN E1M bench, SoC I2C2 @0x50).
 * Fixed by replacing both checks with the shared, subtraction-based
 * alp_size_range_valid() helper (src/common/alp_checked_arith.h, #743).
 *
 * No real I2C bus is needed to exercise the range check: it runs before any
 * bus access, so the tests build an eeprom_24c128_t by hand (bypassing
 * eeprom_24c128_init(), which would probe real hardware) with `bus = NULL`
 * and `initialised = true`.  Passing NULL as the bus is itself part of the
 * proof for the SIZE_MAX cases -- see the comment on
 * test_read_len_size_max_does_not_wrap() below.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_eeprom_24c128_range
 *   ctest --test-dir build -R alp_test_eeprom_24c128_range
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_eeprom_24c128_range:
 *   cmake -B build -DALP_OS=baremetal -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_baremetal_eeprom_24c128_range
 *   ctest --test-dir build -R alp_test_baremetal_eeprom_24c128_range
 */

#include <stdint.h>
#include <string.h>

#include "alp/chips/eeprom_24c128.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static eeprom_24c128_t z_make_ctx(void)
{
	eeprom_24c128_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.initialised = true;
	ctx.bus         = NULL; /* Never dereferenced on the OUT_OF_RANGE path. */
	ctx.addr        = EEPROM_24C128_I2C_ADDR_LOW;
	return ctx;
}

/* offset 0, len 0 -- zero-length request at the very start is valid. */
static void test_read_zero_offset_zero_len(void)
{
	eeprom_24c128_t ctx = z_make_ctx();
	ALP_ASSERT_EQ_INT(eeprom_24c128_read(&ctx, 0u, NULL, 0u), ALP_OK);
}

static void test_write_zero_offset_zero_len(void)
{
	eeprom_24c128_t ctx = z_make_ctx();
	ALP_ASSERT_EQ_INT(eeprom_24c128_write(&ctx, 0u, NULL, 0u), ALP_OK);
}

/* Exact end of device capacity: offset 0, len == EEPROM_24C128_BYTES spans
 * [0, capacity) exactly -- valid.  bus == NULL makes the actual I2C call
 * fail with ALP_ERR_NOT_READY, which is fine: this test only cares that the
 * range check itself accepted the request (i.e. did NOT return
 * ALP_ERR_OUT_OF_RANGE). */
static void test_read_exact_capacity_is_not_out_of_range(void)
{
	uint8_t         out[4] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc     = eeprom_24c128_read(&ctx, 0u, out, EEPROM_24C128_BYTES);
	if (rc == ALP_ERR_OUT_OF_RANGE) {
		ALP_TEST_FAIL("exact-capacity read rejected as out-of-range (rc=%d)", rc);
	} else {
		ALP_TEST_PASS();
	}
}

/* Last valid byte: offset == capacity - 1, len == 1. */
static void test_read_last_valid_byte(void)
{
	uint8_t         out[1] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc = eeprom_24c128_read(&ctx, (uint16_t)(EEPROM_24C128_BYTES - 1u), out, 1u);
	if (rc == ALP_ERR_OUT_OF_RANGE) {
		ALP_TEST_FAIL("last-valid-byte read rejected as out-of-range (rc=%d)", rc);
	} else {
		ALP_TEST_PASS();
	}
}

/* offset == capacity, len == 0: a zero-length request exactly at the end is
 * valid per alp_size_range_valid()'s contract. */
static void test_read_offset_at_capacity_zero_len(void)
{
	eeprom_24c128_t ctx = z_make_ctx();
	ALP_ASSERT_EQ_INT(eeprom_24c128_read(&ctx, (uint16_t)EEPROM_24C128_BYTES, NULL, 0u), ALP_OK);
}

/* One byte past the end: offset == capacity, len == 1. */
static void test_read_one_byte_past_end(void)
{
	uint8_t         out[1] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc     = eeprom_24c128_read(&ctx, (uint16_t)EEPROM_24C128_BYTES, out, 1u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* offset already beyond capacity. */
static void test_read_offset_beyond_capacity(void)
{
	uint8_t         out[1] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc = eeprom_24c128_read(&ctx, (uint16_t)(EEPROM_24C128_BYTES + 1u), out, 1u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* offset + len overflows by 1 (no size_t wrap, just a plain boundary
 * miss) -- offset=1, len=EEPROM_24C128_BYTES spans one byte past the end. */
static void test_read_offset_plus_len_one_past_end(void)
{
	uint8_t         out[4] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc     = eeprom_24c128_read(&ctx, 1u, out, EEPROM_24C128_BYTES);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* The size_t-overflow case (#738's core bug): a small offset with
 * len == SIZE_MAX.  The old `(uint32_t)offset + len > EEPROM_24C128_BYTES`
 * check computes `offset + SIZE_MAX`, which wraps around to `offset - 1`
 * (mod SIZE_MAX+1) -- e.g. offset=100 wraps to 99, which is NOT greater than
 * EEPROM_24C128_BYTES, so the old code treated this as a valid, in-range
 * request and fell through past the range check.
 *
 * With bus == NULL, the fallthrough is still observable without touching
 * real hardware: eeprom_24c128_read() would reach
 * alp_i2c_write_read(ctx->bus, ...), which short-circuits on `bus == NULL`
 * to ALP_ERR_NOT_READY *before* the range check has a chance to run again.
 * So:
 *   - OLD code:  returns ALP_ERR_NOT_READY (range check bypassed, fell
 *                through into the NULL-bus guard further down the stack).
 *   - NEW code:  returns ALP_ERR_OUT_OF_RANGE (range check catches it
 *                immediately, never reaches the I2C call).
 * This test fails against the pre-fix code: it asserts ALP_ERR_OUT_OF_RANGE,
 * but the pre-fix check returns ALP_ERR_NOT_READY instead (confirmed by
 * re-running this file against the pre-fix eeprom_24c128.c locally). */
static void test_read_len_size_max_does_not_wrap(void)
{
	uint8_t         out[4] = { 0 };
	eeprom_24c128_t ctx    = z_make_ctx();
	alp_status_t    rc     = eeprom_24c128_read(&ctx, 100u, out, SIZE_MAX);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* Same SIZE_MAX-wrap case for the write path.  offset MUST be nonzero here --
 * `0 + SIZE_MAX` doesn't wrap (it's already SIZE_MAX, correctly rejected by
 * even the old unsigned-sum check), so offset=0 would not exercise the wrap
 * at all.  offset=100 mirrors the read-path test above: `100 + SIZE_MAX`
 * wraps to 99, which the old check saw as comfortably in-range.  `data` is
 * sized to EEPROM_24C128_PAGE_BYTES so the first (and, given bus == NULL,
 * only) page-splitting memcpy stays in-bounds regardless of offset, should
 * the pre-fix bypass ever reach it. */
static void test_write_len_size_max_does_not_wrap(void)
{
	uint8_t         data[EEPROM_24C128_PAGE_BYTES] = { 0 };
	eeprom_24c128_t ctx                            = z_make_ctx();
	alp_status_t    rc = eeprom_24c128_write(&ctx, 100u, data, SIZE_MAX);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* offset itself is SIZE_MAX-adjacent when widened -- exercises the same
 * helper on the write path with a large-but-finite offset well beyond the
 * uint16_t range boundary the API exposes (offset is uint16_t, so this is
 * really just "offset beyond capacity", included for write-path parity with
 * the read-path checks above). */
static void test_write_offset_beyond_capacity(void)
{
	uint8_t         data[1] = { 0 };
	eeprom_24c128_t ctx     = z_make_ctx();
	alp_status_t    rc = eeprom_24c128_write(&ctx, (uint16_t)(EEPROM_24C128_BYTES + 1u), data, 1u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

int main(void)
{
	test_read_zero_offset_zero_len();
	test_write_zero_offset_zero_len();
	test_read_exact_capacity_is_not_out_of_range();
	test_read_last_valid_byte();
	test_read_offset_at_capacity_zero_len();
	test_read_one_byte_past_end();
	test_read_offset_beyond_capacity();
	test_read_offset_plus_len_one_past_end();
	test_read_len_size_max_does_not_wrap();
	test_write_len_size_max_does_not_wrap();
	test_write_offset_beyond_capacity();

	ALP_TEST_SUMMARY();
}
