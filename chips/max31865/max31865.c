/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Maxim MAX31865 RTD driver.  See <alp/chips/max31865.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/max31865.h"

#define MAX31865_WRITE_BIT 0x80u

static alp_status_t max31865_write_reg(max31865_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { (uint8_t)(reg | MAX31865_WRITE_BIT), val };
	return alp_spi_write(dev->bus, buf, sizeof(buf));
}

alp_status_t max31865_init(max31865_t *dev, alp_spi_t *spi)
{
	if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = spi;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t max31865_set_config(max31865_t *dev, uint8_t config)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return max31865_write_reg(dev, MAX31865_REG_CONFIG, config);
}

alp_status_t max31865_read_rtd(max31865_t *dev, uint16_t *rtd_out, bool *fault_set)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (rtd_out == NULL) return ALP_ERR_INVAL;
	/* One CS window, auto-incrementing 0x01 -> 0x02, so MSB and LSB always
	 * come from the same conversion. */
	uint8_t      tx[3] = { (uint8_t)(MAX31865_REG_RTD_MSB & 0x7Fu), 0x00, 0x00 };
	uint8_t      rx[3] = { 0 };
	alp_status_t s     = alp_spi_transceive(dev->bus, tx, rx, sizeof(tx));
	if (s != ALP_OK) return s;
	const uint8_t msb = rx[1];
	const uint8_t lsb = rx[2];
	if (fault_set != NULL) *fault_set = (lsb & 0x01u) != 0u;
	*rtd_out = (uint16_t)(((uint16_t)msb << 7) | ((lsb & 0xFEu) >> 1));
	return ALP_OK;
}

void max31865_deinit(max31865_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
