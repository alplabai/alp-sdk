/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BMP390 pressure sensor driver (I²C).
 * See <alp/chips/bmp390.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/bmp390.h"

#define BMP390_REG_CMD        0x7Eu
#define BMP390_CMD_SOFT_RESET 0xB6u

alp_status_t bmp390_init(bmp390_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus  = bus;
	dev->addr = i2c_addr;

	uint8_t      reg = BMP390_REG_CHIP_ID;
	uint8_t      id  = 0;
	alp_status_t s   = alp_i2c_write_read(bus, i2c_addr, &reg, 1, &id, 1);
	if (s != ALP_OK) return s;
	if (id != BMP390_CHIP_ID) return ALP_ERR_IO;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t bmp390_read_id(bmp390_t *dev, uint8_t *id_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (id_out == NULL) return ALP_ERR_INVAL;
	uint8_t reg = BMP390_REG_CHIP_ID;
	return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, id_out, 1);
}

alp_status_t bmp390_soft_reset(bmp390_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	uint8_t buf[2] = { BMP390_REG_CMD, BMP390_CMD_SOFT_RESET };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

void bmp390_deinit(bmp390_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
