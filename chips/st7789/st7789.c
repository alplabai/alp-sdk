/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sitronix ST7789V TFT driver.  See <alp/chips/st7789.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/st7789.h"
#include "alp/peripheral.h"

#define ST7789_CMD_SWRESET 0x01u
#define ST7789_CMD_SLPOUT  0x11u
#define ST7789_CMD_COLMOD  0x3Au /* COLour MODe; 0x05 = 16bpp */
#define ST7789_CMD_MADCTL  0x36u
#define ST7789_CMD_INVON   0x21u
#define ST7789_CMD_NORON   0x13u
#define ST7789_CMD_DISPON  0x29u
#define ST7789_CMD_CASET   0x2Au
#define ST7789_CMD_RASET   0x2Bu
#define ST7789_CMD_RAMWR   0x2Cu

static alp_status_t st7789_send(st7789_t *dev, bool is_cmd, const uint8_t *buf, size_t len)
{
	alp_status_t s = alp_gpio_write(dev->dc, !is_cmd);
	if (s != ALP_OK) return s;
	return alp_spi_write(dev->bus, buf, len);
}

static alp_status_t st7789_cmd(st7789_t *dev, uint8_t c)
{
	return st7789_send(dev, true, &c, 1);
}

static alp_status_t st7789_data(st7789_t *dev, const uint8_t *buf, size_t len)
{
	return st7789_send(dev, false, buf, len);
}

alp_status_t st7789_init(st7789_t   *dev,
                         alp_spi_t  *spi,
                         alp_gpio_t *dc,
                         alp_gpio_t *reset,
                         uint16_t    width,
                         uint16_t    height)
{
	if (dev == NULL || spi == NULL || dc == NULL) return ALP_ERR_INVAL;
	if (width == 0 || width > ST7789_MAX_WIDTH) return ALP_ERR_INVAL;
	if (height == 0 || height > ST7789_MAX_HEIGHT) return ALP_ERR_INVAL;

	memset(dev, 0, sizeof(*dev));
	dev->bus    = spi;
	dev->dc     = dc;
	dev->reset  = reset;
	dev->width  = width;
	dev->height = height;

	if (reset != NULL) {
		(void)alp_gpio_write(reset, false);
		alp_delay_us(20000);
		(void)alp_gpio_write(reset, true);
		alp_delay_us(150000);
	}

	alp_status_t s = st7789_cmd(dev, ST7789_CMD_SWRESET);
	if (s != ALP_OK) return s;
	alp_delay_us(150000);
	s = st7789_cmd(dev, ST7789_CMD_SLPOUT);
	if (s != ALP_OK) return s;
	alp_delay_us(500000);

	uint8_t colmod = 0x55; /* 16-bit/pixel RGB565 */
	s              = st7789_cmd(dev, ST7789_CMD_COLMOD);
	if (s != ALP_OK) return s;
	s = st7789_data(dev, &colmod, 1);
	if (s != ALP_OK) return s;

	uint8_t madctl = 0x00;
	s              = st7789_cmd(dev, ST7789_CMD_MADCTL);
	if (s != ALP_OK) return s;
	s = st7789_data(dev, &madctl, 1);
	if (s != ALP_OK) return s;

	s = st7789_cmd(dev, ST7789_CMD_INVON);
	if (s != ALP_OK) return s;
	s = st7789_cmd(dev, ST7789_CMD_NORON);
	if (s != ALP_OK) return s;
	s = st7789_cmd(dev, ST7789_CMD_DISPON);
	if (s != ALP_OK) return s;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t st7789_set_window(st7789_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (x1 <= x0 || y1 <= y0) return ALP_ERR_INVAL;
	if (x1 >= dev->width || y1 >= dev->height) return ALP_ERR_INVAL;

	uint8_t buf[4] = {
		(uint8_t)(x0 >> 8),
		(uint8_t)(x0 & 0xFF),
		(uint8_t)(x1 >> 8),
		(uint8_t)(x1 & 0xFF),
	};
	alp_status_t s = st7789_cmd(dev, ST7789_CMD_CASET);
	if (s != ALP_OK) return s;
	s = st7789_data(dev, buf, sizeof(buf));
	if (s != ALP_OK) return s;

	buf[0] = (uint8_t)(y0 >> 8);
	buf[1] = (uint8_t)(y0 & 0xFF);
	buf[2] = (uint8_t)(y1 >> 8);
	buf[3] = (uint8_t)(y1 & 0xFF);
	s      = st7789_cmd(dev, ST7789_CMD_RASET);
	if (s != ALP_OK) return s;
	return st7789_data(dev, buf, sizeof(buf));
}

alp_status_t st7789_write_pixels(st7789_t *dev, const uint8_t *pixels, size_t n_bytes)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (pixels == NULL) return ALP_ERR_INVAL;
	if ((n_bytes & 1u) != 0) return ALP_ERR_INVAL;

	alp_status_t s = st7789_cmd(dev, ST7789_CMD_RAMWR);
	if (s != ALP_OK) return s;
	return st7789_data(dev, pixels, n_bytes);
}

void st7789_deinit(st7789_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
	dev->dc          = NULL;
	dev->reset       = NULL;
}
