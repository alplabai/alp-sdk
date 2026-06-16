/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ams TSL2591 light sensor driver.  See <alp/chips/tsl2591.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tsl2591.h"

static alp_status_t tsl2591_read_reg(tsl2591_t *dev, uint8_t reg, uint8_t *val)
{
	uint8_t cmd = (uint8_t)(TSL2591_CMD_BIT | reg);
	return alp_i2c_write_read(dev->bus, dev->addr, &cmd, 1, val, 1);
}

static alp_status_t tsl2591_write_reg(tsl2591_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { (uint8_t)(TSL2591_CMD_BIT | reg), val };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t tsl2591_init(tsl2591_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus        = bus;
	dev->addr       = i2c_addr;

	uint8_t      id = 0;
	alp_status_t s  = tsl2591_read_reg(dev, TSL2591_REG_ID, &id);
	if (s != ALP_OK) return s;
	if (id != TSL2591_DEVICE_ID) return ALP_ERR_IO;

	dev->initialised = true;
	/* Power on + ALS enable; integration / gain defaults are OK for v0.5. */
	return tsl2591_write_reg(dev, TSL2591_REG_ENABLE, TSL2591_ENABLE_POWER_ON | TSL2591_ENABLE_AEN);
}

alp_status_t tsl2591_read_channels(tsl2591_t *dev, uint16_t *ch0_full_out, uint16_t *ch1_ir_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (ch0_full_out == NULL || ch1_ir_out == NULL) return ALP_ERR_INVAL;

	uint8_t      cmd    = (uint8_t)(TSL2591_CMD_BIT | TSL2591_REG_C0DATA_LO);
	uint8_t      buf[4] = { 0 };
	alp_status_t s      = alp_i2c_write_read(dev->bus, dev->addr, &cmd, 1, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	*ch0_full_out = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
	*ch1_ir_out   = (uint16_t)(((uint16_t)buf[3] << 8) | buf[2]);
	return ALP_OK;
}

void tsl2591_deinit(tsl2591_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
