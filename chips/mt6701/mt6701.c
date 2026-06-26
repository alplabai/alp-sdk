/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MagnTek MT6701 magnetic encoder driver (I²C).
 * See <alp/chips/mt6701.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/mt6701.h"

alp_status_t mt6701_init(mt6701_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t mt6701_read_angle(mt6701_t *dev, uint16_t *angle_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (angle_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reg    = MT6701_REG_ANGLE_HI;
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	/* HI: 8 MSBs; LO: 6 LSBs in bits 7..2. */
	*angle_out = (uint16_t)(((uint16_t)buf[0] << 6) | ((buf[1] >> 2) & 0x3Fu));
	return ALP_OK;
}

void mt6701_deinit(mt6701_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
