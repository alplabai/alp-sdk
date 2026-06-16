/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Maxim MAX31855 K-type thermocouple driver.
 * See <alp/chips/max31855.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/max31855.h"

alp_status_t max31855_init(max31855_t *dev, alp_spi_t *spi)
{
	if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = spi;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t
max31855_read(max31855_t *dev, int32_t *tc_milli_c, int32_t *internal_milli_c, uint8_t *fault_flags)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	uint8_t      rx[4] = { 0 };
	alp_status_t s     = alp_spi_transceive(dev->bus, NULL, rx, sizeof(rx));
	if (s != ALP_OK) return s;

	uint32_t word = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) |
	                (uint32_t)rx[3];

	/* Bits 31..18: thermocouple (signed 14-bit, 0.25 C/LSB).
     * Bits 17 reserved.
     * Bit 16: fault (any).
     * Bits 15..4: internal cold-junction (signed 12-bit, 0.0625 C/LSB).
     * Bit  3 reserved.
     * Bits 2..0: OC / SCG / SCV. */
	if (fault_flags != NULL) {
		*fault_flags = (uint8_t)(word & 0x07u) | ((word & 0x10000u) != 0u ? 0x10u : 0u);
	}
	if (tc_milli_c != NULL) {
		int16_t tc14 = (int16_t)((word >> 16) & 0xFFFCu); /* clear lo 2 bits = reserved+fault */
		int32_t sx   = (int32_t)tc14 / 4;                 /* 14-bit signed shift -> int32 */
		*tc_milli_c  = sx * 250;                          /* 0.25 C/LSB -> 250 milli-C/LSB */
	}
	if (internal_milli_c != NULL) {
		int16_t ic16      = (int16_t)((word & 0xFFF0u));
		int32_t sx        = (int32_t)ic16 / 16; /* 12-bit signed shift */
		*internal_milli_c = (sx * 1000) / 16;   /* 0.0625 C/LSB */
	}
	return ALP_OK;
}

void max31855_deinit(max31855_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
