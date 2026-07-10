/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux spidev backend
 * (src/yocto/peripheral_spi.c).
 *
 * Scope mirrors tests/yocto/peripheral_i2c.c: failure paths that
 * don't need a real SPI adapter or kernel-mode spidev test
 * fixture.  Real-adapter happy-path coverage is parked behind
 * docs/ci/HW-IN-LOOP.md until the `hil-yocto` self-hosted runner
 * lands -- the on-device plan reads the SPI flash's JEDEC ID
 * (0x9F READID) as the canonical first SPI transaction.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_spi
 *   ctest --test-dir build -R alp_test_peripheral_spi
 */

#include <stdint.h>

#include "alp/peripheral.h"

#include "test_assert.h"

/* /dev/spidev999.0 will not exist on any sane CI runner. */
#define ALP_TEST_BUS_NONEXISTENT 999u

static void test_null_cfg_returns_null_and_stamps_invalid(void)
{
	alp_spi_t *bus = alp_spi_open(NULL);
	ALP_ASSERT_NULL(bus);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_mode_returns_null_and_stamps_invalid(void)
{
	alp_spi_config_t cfg = {
		.bus_id        = 0,
		.cs_pin_id     = 0,
		.freq_hz       = 1000000,
		.mode          = (alp_spi_mode_t)42, /* outside MODE_0..MODE_3 */
		.bits_per_word = 8,
	};
	alp_spi_t *bus = alp_spi_open(&cfg);
	ALP_ASSERT_NULL(bus);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_bits_returns_null_and_stamps_invalid(void)
{
	alp_spi_config_t cfg = {
		.bus_id        = 0,
		.cs_pin_id     = 0,
		.freq_hz       = 1000000,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 64, /* spidev tops out at 32 */
	};
	alp_spi_t *bus = alp_spi_open(&cfg);
	ALP_ASSERT_NULL(bus);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_nonexistent_bus_returns_null_and_stamps_not_ready(void)
{
	alp_spi_config_t cfg = {
		.bus_id        = ALP_TEST_BUS_NONEXISTENT,
		.cs_pin_id     = 0,
		.freq_hz       = 1000000,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 8,
	};
	alp_spi_t *bus = alp_spi_open(&cfg);
	ALP_ASSERT_NULL(bus);
	/* ENOENT -> ALP_ERR_NOT_READY per the errno_to_alp mapping. */
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

/* #606: ALP_SPI_NO_CS used to be formatted verbatim into the spidev
 * path ("/dev/spidev<bus>.4294967295"), which always failed with a
 * confusing ENOENT instead of an explicit NOSUPPORT.  Assert the
 * sentinel is rejected before any path is ever opened -- a bogus bus
 * id (ALP_TEST_BUS_NONEXISTENT) proves this, since the ENOENT path
 * above would otherwise also surface (as ALP_ERR_NOT_READY, not
 * NOSUPPORT) if NO_CS handling were skipped. */
static void test_no_cs_returns_null_and_stamps_nosupport(void)
{
	alp_spi_config_t cfg = {
		.bus_id        = ALP_TEST_BUS_NONEXISTENT,
		.cs_pin_id     = ALP_SPI_NO_CS,
		.freq_hz       = 1000000,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 8,
	};
	alp_spi_t *bus = alp_spi_open(&cfg);
	ALP_ASSERT_NULL(bus);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

/* Ordinary (non-sentinel) out-of-range CS ids still take the normal
 * path -- they're a valid-looking spidev minor that simply doesn't
 * exist on this host, so they fail the same way bus_id does: ENOENT ->
 * ALP_ERR_NOT_READY, never NOSUPPORT.  This is the "out-of-range IDs"
 * case from the #606 acceptance criteria, distinct from the NO_CS
 * sentinel above. */
static void test_out_of_range_cs_is_not_confused_with_no_cs(void)
{
	alp_spi_config_t cfg = {
		.bus_id        = 0,
		.cs_pin_id     = ALP_TEST_BUS_NONEXISTENT, /* a plausible but nonexistent CS index */
		.freq_hz       = 1000000,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 8,
	};
	alp_spi_t *bus = alp_spi_open(&cfg);
	ALP_ASSERT_NULL(bus);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

static void test_transceive_on_null_bus_returns_invalid(void)
{
	uint8_t      tx[1] = { 0x9F };
	uint8_t      rx[3] = { 0 };
	alp_status_t rc    = alp_spi_transceive(NULL, tx, rx, sizeof(tx));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_write_on_null_bus_returns_invalid(void)
{
	uint8_t      buf[1] = { 0x00 };
	alp_status_t rc     = alp_spi_write(NULL, buf, sizeof(buf));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_on_null_bus_returns_invalid(void)
{
	uint8_t      buf[1] = { 0 };
	alp_status_t rc     = alp_spi_read(NULL, buf, sizeof(buf));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
	alp_spi_close(NULL);
	ALP_TEST_PASS();
}

int main(void)
{
	test_null_cfg_returns_null_and_stamps_invalid();
	test_invalid_mode_returns_null_and_stamps_invalid();
	test_invalid_bits_returns_null_and_stamps_invalid();
	test_nonexistent_bus_returns_null_and_stamps_not_ready();
	test_no_cs_returns_null_and_stamps_nosupport();
	test_out_of_range_cs_is_not_confused_with_no_cs();
	test_transceive_on_null_bus_returns_invalid();
	test_write_on_null_bus_returns_invalid();
	test_read_on_null_bus_returns_invalid();
	test_close_null_is_safe();

	ALP_TEST_SUMMARY();
}
