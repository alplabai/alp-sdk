/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sino Wealth SH1106 OLED driver.  See <alp/chips/sh1106.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/sh1106.h"

#define SH1106_CTRL_CMD 0x00u
#define SH1106_CTRL_DATA 0x40u

#define SH1106_CMD_DISP_OFF 0xAEu
#define SH1106_CMD_DISP_ON 0xAFu
#define SH1106_CMD_SET_PAGE_BASE 0xB0u
#define SH1106_CMD_SET_COL_LOW 0x00u  /* OR with low nibble. */
#define SH1106_CMD_SET_COL_HIGH 0x10u /* OR with high nibble. */
#define SH1106_CMD_NORMAL_DISPLAY 0xA6u

/* SH1106 visible 128 columns are mapped at internal column 2..129. */
#define SH1106_COL_OFFSET 2

static alp_status_t sh1106_write_cmd(sh1106_t *dev, uint8_t c)
{
	uint8_t buf[2] = { SH1106_CTRL_CMD, c };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t sh1106_init(sh1106_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus       = bus;
	dev->addr      = i2c_addr;

	alp_status_t s = sh1106_write_cmd(dev, SH1106_CMD_DISP_OFF);
	if (s != ALP_OK) return s;
	s = sh1106_write_cmd(dev, SH1106_CMD_NORMAL_DISPLAY);
	if (s != ALP_OK) return s;
	s = sh1106_write_cmd(dev, SH1106_CMD_DISP_ON);
	if (s != ALP_OK) return s;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t sh1106_set_display_on(sh1106_t *dev, bool on)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return sh1106_write_cmd(dev, on ? SH1106_CMD_DISP_ON : SH1106_CMD_DISP_OFF);
}

void sh1106_clear(sh1106_t *dev)
{
	if (dev == NULL) return;
	memset(dev->fb, 0, sizeof(dev->fb));
}

void sh1106_draw_pixel(sh1106_t *dev, uint16_t x, uint16_t y, bool on)
{
	if (dev == NULL) return;
	if (x >= SH1106_WIDTH || y >= SH1106_HEIGHT) return;
	const size_t  page   = y / 8u;
	const uint8_t bit    = (uint8_t)(1u << (y % 8u));
	const size_t  fb_idx = page * SH1106_WIDTH + x;
	if (fb_idx >= sizeof(dev->fb)) return;
	if (on)
		dev->fb[fb_idx] |= bit;
	else
		dev->fb[fb_idx] &= (uint8_t)~bit;
}

alp_status_t sh1106_display(sh1106_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	for (uint8_t page = 0; page < 8; page++) {
		alp_status_t s = sh1106_write_cmd(dev, (uint8_t)(SH1106_CMD_SET_PAGE_BASE | page));
		if (s != ALP_OK) return s;
		s = sh1106_write_cmd(dev, (uint8_t)(SH1106_CMD_SET_COL_LOW | (SH1106_COL_OFFSET & 0x0F)));
		if (s != ALP_OK) return s;
		s = sh1106_write_cmd(
		    dev, (uint8_t)(SH1106_CMD_SET_COL_HIGH | ((SH1106_COL_OFFSET >> 4) & 0x0F)));
		if (s != ALP_OK) return s;

		/* Push one full page (128 bytes) prefixed with a data control byte. */
		uint8_t buf[1 + SH1106_WIDTH];
		buf[0] = SH1106_CTRL_DATA;
		memcpy(&buf[1], &dev->fb[(size_t)page * SH1106_WIDTH], SH1106_WIDTH);
		s = alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
		if (s != ALP_OK) return s;
	}
	return ALP_OK;
}

void sh1106_deinit(sh1106_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
