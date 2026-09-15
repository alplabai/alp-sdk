/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit checks for eeprom_24c128_secure_page_write() and
 * eeprom_24c128_secure_page_lock() (both [UNTESTED] / [PAPER-ONLY] against
 * silicon -- see the header's @par Verification status block).
 *
 * Same bus=NULL-as-NACK-double technique as
 * tests/common/eeprom_24c128_read_identity.c: neither the Yocto nor the
 * baremetal test layer links a fake I2C backend, so the actual wire bytes
 * these two functions put on 0x58 are not observable from this layer --
 * that is covered instead by a real bench run before the first production
 * lock. What IS checkable here is the argument/state contract (NULL /
 * uninitialised refusals) and that a bus failure propagates rather than
 * being swallowed.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_eeprom_24c128_secure_page
 *   ctest --test-dir build -R alp_test_eeprom_24c128_secure_page
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_eeprom_24c128_secure_page.
 */

#include <stdint.h>
#include <string.h>

#include "alp/chips/eeprom_24c128.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static eeprom_24c128_t z_make_ctx(uint8_t addr, bool initialised)
{
	eeprom_24c128_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.initialised = initialised;
	ctx.bus         = NULL; /* Never dereferenced by a well-behaved backend
                              * before it short-circuits on NULL -- see this
                              * file's header comment. */
	ctx.addr        = addr;
	return ctx;
}

/* ---- eeprom_24c128_secure_page_write ---- */

static void test_write_null_ctx(void)
{
	uint8_t page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(NULL, page), ALP_ERR_NOT_READY);
}

static void test_write_null_data(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, true);
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(&ctx, NULL), ALP_ERR_INVAL);
}

static void test_write_uninitialised_ctx(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, false);
	uint8_t         page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(&ctx, page), ALP_ERR_NOT_READY);
}

/* bus == NULL propagates as a bus-layer failure rather than being silently
 * swallowed as ALP_OK -- a write that "succeeds" against no bus at all
 * would be the worst possible bug in this specific function, given what it
 * gates (see eeprom_24c128_secure_page_lock's doc comment). */
static void test_write_nack_equivalent_propagates_error(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, true);
	uint8_t         page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));
	alp_status_t rc = eeprom_24c128_secure_page_write(&ctx, page);
	ALP_ASSERT_TRUE(rc != ALP_OK);
}

/* ---- eeprom_24c128_secure_page_lock ---- */

static void test_lock_null_ctx(void)
{
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_lock(NULL), ALP_ERR_NOT_READY);
}

static void test_lock_uninitialised_ctx(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, false);
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_lock(&ctx), ALP_ERR_NOT_READY);
}

/* Same shape as test_write_nack_equivalent_propagates_error: this is the
 * PERMANENT, IRREVERSIBLE call -- it must never report success against a
 * bus that never actually spoke. */
static void test_lock_nack_equivalent_propagates_error(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, true);
	alp_status_t    rc  = eeprom_24c128_secure_page_lock(&ctx);
	ALP_ASSERT_TRUE(rc != ALP_OK);
}

int main(void)
{
	test_write_null_ctx();
	test_write_null_data();
	test_write_uninitialised_ctx();
	test_write_nack_equivalent_propagates_error();

	test_lock_null_ctx();
	test_lock_uninitialised_ctx();
	test_lock_nack_equivalent_propagates_error();

	ALP_TEST_SUMMARY();
}
