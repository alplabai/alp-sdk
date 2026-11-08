/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2986: tsl2591_init() used to set dev->initialised = true before the
 * ENABLE write, so a failed write left the context "ready" while the
 * ADC was still powered down and tsl2591_read_channels() returned
 * ALP_OK with 0/0 counts.  The flag is now set only after the write
 * lands.
 *
 * Links the REAL chips/tsl2591/tsl2591.c TU against an in-TU fake of
 * alp_i2c_write() / alp_i2c_write_read() (the only two bus symbols it
 * calls; alp_i2c_t is opaque), as tests/unit/rv3028c7_dispatch_ack does.
 */
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/ztest.h>

#include "alp/chips/tsl2591.h"

static alp_status_t write_result;
static unsigned     write_calls;

alp_status_t alp_i2c_write_read(alp_i2c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *tx,
                                size_t         txlen,
                                uint8_t       *rx,
                                size_t         rxlen)
{
	(void)bus;
	(void)addr;
	(void)txlen;
	/* Only the ID register read must succeed; any other read is the
	 * channel-data read, which returns zeroes. */
	uint8_t reg = (uint8_t)(tx[0] & ~TSL2591_CMD_BIT);
	for (size_t i = 0; i < rxlen; ++i)
		rx[i] = (reg == TSL2591_REG_ID) ? TSL2591_DEVICE_ID : 0;
	return ALP_OK;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *tx, size_t txlen)
{
	(void)bus;
	(void)addr;
	(void)tx;
	(void)txlen;
	write_calls++;
	return write_result;
}

static void before(void *fixture)
{
	(void)fixture;
	write_result = ALP_OK;
	write_calls  = 0;
}

ZTEST_SUITE(tsl2591_init_enable_fail, NULL, NULL, before, NULL, NULL);

ZTEST(tsl2591_init_enable_fail, test_failed_enable_write_leaves_driver_uninitialised)
{
	tsl2591_t dev;
	uint16_t  ch0 = 0xAAAA, ch1 = 0xBBBB;

	write_result = ALP_ERR_IO;

	zassert_equal(
	    tsl2591_init(&dev, (alp_i2c_t *)1, 0x29), ALP_ERR_IO, "init must report the write error");
	zassert_equal(write_calls, 1u, "ENABLE write not attempted");
	zassert_false(dev.initialised, "initialised set despite failed ENABLE write");
	zassert_equal(tsl2591_read_channels(&dev, &ch0, &ch1),
	              ALP_ERR_NOT_READY,
	              "read must be gated, not return 0/0 counts");
	zassert_equal(ch0, 0xAAAAu, "ch0 touched");
	zassert_equal(ch1, 0xBBBBu, "ch1 touched");
}

ZTEST(tsl2591_init_enable_fail, test_successful_init_is_readable)
{
	tsl2591_t dev;
	uint16_t  ch0, ch1;

	zassert_equal(tsl2591_init(&dev, (alp_i2c_t *)1, 0x29), ALP_OK, "init failed");
	zassert_true(dev.initialised, "initialised not set after successful init");
	zassert_equal(tsl2591_read_channels(&dev, &ch0, &ch1), ALP_OK, "read failed");
}
