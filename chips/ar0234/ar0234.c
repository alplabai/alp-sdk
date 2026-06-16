/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * onsemi AR0234CS 1080p global-shutter MIPI CSI-2 driver (SCCB).
 * See <alp/chips/ar0234.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ar0234.h"

#define AR0234_REG_RESET 0x301Au

static alp_status_t ar0234_read16(ar0234_t *dev, uint16_t reg, uint16_t *val)
{
	uint8_t      addr_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
	uint8_t      buf[2]     = { 0 };
	alp_status_t s = alp_i2c_write_read(dev->bus, dev->addr, addr_be, sizeof(addr_be), buf, 2);
	if (s != ALP_OK) return s;
	*val = ((uint16_t)buf[0] << 8) | buf[1];
	return ALP_OK;
}

static alp_status_t ar0234_write16(ar0234_t *dev, uint16_t reg, uint16_t val)
{
	uint8_t buf[4] = {
		(uint8_t)(reg >> 8),
		(uint8_t)(reg & 0xFF),
		(uint8_t)(val >> 8),
		(uint8_t)(val & 0xFF),
	};
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t ar0234_init(ar0234_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	memset(dev, 0, sizeof(*dev));
	dev->bus        = bus;
	dev->addr       = i2c_addr;

	uint16_t     id = 0;
	alp_status_t s  = ar0234_read16(dev, AR0234_REG_CHIP_VERSION, &id);
	if (s != ALP_OK) return s;
	if (id != AR0234_CHIP_ID) return ALP_ERR_IO;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t ar0234_read_id(ar0234_t *dev, uint16_t *id_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (id_out == NULL) return ALP_ERR_INVAL;
	return ar0234_read16(dev, AR0234_REG_CHIP_VERSION, id_out);
}

alp_status_t ar0234_soft_reset(ar0234_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return ar0234_write16(dev, AR0234_REG_RESET, 0x0001);
}

void ar0234_deinit(ar0234_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
