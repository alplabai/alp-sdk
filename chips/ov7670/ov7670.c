/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OmniVision OV7670 VGA DVP image sensor driver (SCCB side).
 * See <alp/chips/ov7670.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ov7670.h"

#define OV7670_REG_COM7 0x12u /* bit 7 = soft reset. */

static alp_status_t ov7670_read_reg(ov7670_t *dev, uint8_t reg, uint8_t *val)
{
	return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, val, 1);
}

static alp_status_t ov7670_write_reg(ov7670_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t ov7670_init(ov7670_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	memset(dev, 0, sizeof(*dev));
	dev->bus  = bus;
	dev->addr = i2c_addr;

	uint8_t      pid = 0, ver = 0;
	alp_status_t s = ov7670_read_reg(dev, OV7670_REG_PID, &pid);
	if (s != ALP_OK) return s;
	s = ov7670_read_reg(dev, OV7670_REG_VER, &ver);
	if (s != ALP_OK) return s;
	uint16_t id = ((uint16_t)pid << 8) | ver;
	if (id != OV7670_CHIP_ID) return ALP_ERR_IO;

	dev->res         = OV7670_RES_VGA;
	dev->fmt         = OV7670_FMT_YUV422;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t ov7670_read_id(ov7670_t *dev, uint16_t *id_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (id_out == NULL) return ALP_ERR_INVAL;

	uint8_t      pid = 0, ver = 0;
	alp_status_t s = ov7670_read_reg(dev, OV7670_REG_PID, &pid);
	if (s != ALP_OK) return s;
	s = ov7670_read_reg(dev, OV7670_REG_VER, &ver);
	if (s != ALP_OK) return s;
	*id_out = ((uint16_t)pid << 8) | ver;
	return ALP_OK;
}

alp_status_t ov7670_soft_reset(ov7670_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return ov7670_write_reg(dev, OV7670_REG_COM7, 0x80);
}

alp_status_t ov7670_set_resolution(ov7670_t *dev, ov7670_resolution_t res)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if ((int)res < 0 || (int)res > (int)OV7670_RES_VGA) return ALP_ERR_INVAL;
	dev->res = res;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t ov7670_set_format(ov7670_t *dev, ov7670_format_t fmt)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if ((int)fmt < 0 || (int)fmt > (int)OV7670_FMT_BAYER) return ALP_ERR_INVAL;
	dev->fmt = fmt;
	return ALP_ERR_NOSUPPORT;
}

void ov7670_deinit(ov7670_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
