/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ams AS5048B magnetic rotary encoder driver (I²C).
 * See <alp/chips/as5048a_b.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/as5048a_b.h"

alp_status_t as5048b_init(as5048b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t as5048b_read_angle(as5048b_t *dev, uint16_t *angle_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (angle_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reg    = AS5048B_REG_ANGLE_HI;
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	/* HI register: 8 MSBs; LO: 6 LSBs in bits 5..0. */
	*angle_out = (uint16_t)(((uint16_t)buf[0] << 6) | (buf[1] & 0x3Fu));
	return ALP_OK;
}

void as5048b_deinit(as5048b_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
