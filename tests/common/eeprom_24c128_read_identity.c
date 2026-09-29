/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit checks for eeprom_24c128_read_identity() (silicon-verified 2026-09-06
 * on an E1M-AEN803, see the header's @par Verification status block).
 *
 * Like tests/common/eeprom_24c128_range.c, this builds an eeprom_24c128_t by
 * hand (bypassing eeprom_24c128_init(), which would probe real hardware).
 * `bus = NULL` stands in for a NACK: alp_i2c_write_read() short-circuits to
 * ALP_ERR_NOT_READY on a NULL bus before it ever looks at the address it
 * was given (src/i2c_dispatch.c, src/yocto/peripheral_i2c.c both check
 * `bus == NULL` first), which is exactly the observable behaviour a NACKing
 * M24128-BFMH6TG (the DNP alternate part -- no second device-select header)
 * would produce.  That makes bus=NULL the right double for the
 * per-field-`_valid`-not-all-or-nothing contract without a live bus.
 *
 * Neither tests/yocto/ nor tests/baremetal/ link a fake I2C backend (the
 * backend-registry sw_fallback/testing_drv doubles under src/backends/i2c/
 * are wired only into the Zephyr build, see zephyr/CMakeLists.txt), so the
 * actual byte written on the wire for the alt address is not observable
 * from this test layer without inventing new test infrastructure -- out of
 * scope here.  So this file does NOT claim to check the address the driver
 * puts on the wire -- that is covered by the silicon run recorded in the
 * header's @par Verification status block.  What it checks is the argument
 * and state contract (NULL ctx, NULL out, uninitialised ctx), the
 * per-field-`_valid`-not-all-or-nothing behaviour on a NACKing part, and the
 * public alt-address offset CONSTANT.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_eeprom_24c128_read_identity
 *   ctest --test-dir build -R alp_test_eeprom_24c128_read_identity
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_eeprom_24c128_read_identity.
 */

#include <stdint.h>
#include <string.h>

#include "alp/chips/eeprom_24c128.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static eeprom_24c128_t z_make_ctx(uint8_t addr)
{
	eeprom_24c128_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.initialised = true;
	ctx.bus         = NULL; /* Never dereferenced -- NULL short-circuits first. */
	ctx.addr        = addr;
	return ctx;
}

static void test_null_ctx(void)
{
	eeprom_24c128_identity_t out;
	ALP_ASSERT_EQ_INT(eeprom_24c128_read_identity(NULL, &out), ALP_ERR_INVAL);
}

static void test_null_out(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW);
	ALP_ASSERT_EQ_INT(eeprom_24c128_read_identity(&ctx, NULL), ALP_ERR_INVAL);
}

static void test_uninitialised_ctx(void)
{
	eeprom_24c128_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.initialised = false;
	ctx.addr        = EEPROM_24C128_I2C_ADDR_LOW;

	eeprom_24c128_identity_t out;
	ALP_ASSERT_EQ_INT(eeprom_24c128_read_identity(&ctx, &out), ALP_ERR_NOT_READY);
}

/* bus == NULL is the NACK-equivalent double (see file header comment): every
 * one of the four second-header reads fails, so the function must still
 * return ALP_OK with every _valid flag false -- exactly the M24128-BFMH6TG
 * (no second header) contract, not an I/O error. */
static void test_nack_equivalent_all_invalid(void)
{
	eeprom_24c128_t          ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW);
	eeprom_24c128_identity_t out;
	/* Poison with a non-zero pattern first so a field left untouched by a
	 * buggy implementation (i.e. the required memset(out, 0, ...) is
	 * missing) would show up as nonzero below instead of accidentally
	 * reading back as zero anyway. */
	memset(&out, 0xAA, sizeof(out));

	alp_status_t rc = eeprom_24c128_read_identity(&ctx, &out);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT(out.secure_page_valid, false);
	ALP_ASSERT_EQ_INT(out.unique_id_valid, false);
	ALP_ASSERT_EQ_INT(out.lock_valid, false);
	ALP_ASSERT_EQ_INT(out.device_config_valid, false);
	ALP_ASSERT_EQ_INT(out.secure_page_locked, false);
	ALP_ASSERT_EQ_INT(out.device_config, 0);

	/* The four bools plus device_config are only 4 of the 84 poisoned bytes.
	 * The two arrays are the other 80, and they are where a missing
	 * memset(out, 0, sizeof(*out)) would actually hide: delete that memset
	 * and every assertion above still passes, because the driver writes all
	 * five scalars unconditionally.  Check the arrays byte-by-byte so the
	 * poison can do the job its own comment claims. */
	for (size_t i = 0; i < sizeof(out.secure_page); ++i) {
		if (out.secure_page[i] != 0u) {
			ALP_TEST_FAIL("secure_page[%zu] = 0x%02X, expected 0 -- the 0xAA "
			              "poison survived, so out was not zeroed on entry",
			              i,
			              out.secure_page[i]);
			return;
		}
	}
	ALP_TEST_PASS();
	for (size_t i = 0; i < sizeof(out.unique_id); ++i) {
		if (out.unique_id[i] != 0u) {
			ALP_TEST_FAIL("unique_id[%zu] = 0x%02X, expected 0 -- the 0xAA "
			              "poison survived, so out was not zeroed on entry",
			              i,
			              out.unique_id[i]);
			return;
		}
	}
	ALP_TEST_PASS();
}

/* Pin the public alt-address offset constant.
 *
 * This deliberately does NOT re-derive `addr + EEPROM_24C128_ALT_ADDR_OFFSET`
 * and then assert it equals `addr + 0x08`.  An earlier version of this test did
 * exactly that across 0x50..0x57 and was a tautology: it never called
 * eeprom_24c128_read_identity(), so changing the driver to use ctx->addr with no
 * offset at all, or +0x10, left it green.  A check that cannot fail is worse
 * than no check, because it reads as coverage.
 *
 * What is genuinely checkable from this layer is the CONSTANT: 0x08 is the gap
 * between device-select header 1010 (the array, 0x50..0x57) and header 1011
 * (the identity objects, 0x58..0x5F) at identical A2/A1/A0 straps, so it is a
 * fixed property of the N24S128 and not a tuning knob.  If someone changes the
 * macro, this fails.
 *
 * That the DRIVER actually puts that address on the wire is not observable
 * here -- see this file's header comment for why -- and is covered instead by
 * the silicon run recorded in include/alp/chips/eeprom_24c128.h's @par
 * Verification status block, where all four objects answered at 0x58 on a part
 * whose array is at 0x50. */
static void test_alt_address_offset_constant(void)
{
	ALP_ASSERT_EQ_INT(EEPROM_24C128_ALT_ADDR_OFFSET, 0x08u);
	ALP_ASSERT_EQ_INT(EEPROM_24C128_I2C_ADDR_LOW + EEPROM_24C128_ALT_ADDR_OFFSET, 0x58u);
}

int main(void)
{
	test_null_ctx();
	test_null_out();
	test_uninitialised_ctx();
	test_nack_equivalent_all_invalid();
	test_alt_address_offset_constant();

	ALP_TEST_SUMMARY();
}
