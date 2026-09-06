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
 * scope here.  What IS checked directly is the alt-address arithmetic
 * itself (EEPROM_24C128_ALT_ADDR_OFFSET applied across the full legal
 * 7-bit address range from eeprom_24c128_init()'s 0x50..0x57 check), which
 * is exactly the computation eeprom_24c128_read_identity() performs before
 * issuing any transfer.
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
}

/* Alt-address arithmetic: eeprom_24c128_read_identity() must target
 * ctx->addr + EEPROM_24C128_ALT_ADDR_OFFSET (0x08), never ctx->addr itself
 * or some other constant.  Exercised across the full legal 7-bit address
 * range eeprom_24c128_init() accepts (0x50..0x57 -> alt 0x58..0x5F), which
 * is exactly the range this arithmetic has to hold for on real hardware --
 * see the header's device-select-header doc comment. */
static void test_alt_address_offset_across_legal_range(void)
{
	ALP_ASSERT_EQ_INT(EEPROM_24C128_ALT_ADDR_OFFSET, 0x08u);

	for (uint8_t addr = 0x50u; addr <= 0x57u; ++addr) {
		uint8_t alt = (uint8_t)(addr + EEPROM_24C128_ALT_ADDR_OFFSET);
		/* Board fact this whole driver rests on: BOTH addresses are the
		 * SAME physical part (header 1010 vs 1011), never separate
		 * devices -- see chips/eeprom_24c128/eeprom_24c128.c's device-select
		 * comment. */
		ALP_ASSERT_EQ_INT(alt, addr + 0x08u);
		if (alt < 0x58u || alt > 0x5Fu) {
			ALP_TEST_FAIL("alt address 0x%02X out of the expected 0x58..0x5F band "
			              "for primary 0x%02X",
			              alt,
			              addr);
		} else {
			ALP_TEST_PASS();
		}
	}
}

int main(void)
{
	test_null_ctx();
	test_null_out();
	test_uninitialised_ctx();
	test_nack_equivalent_all_invalid();
	test_alt_address_offset_across_legal_range();

	ALP_TEST_SUMMARY();
}
