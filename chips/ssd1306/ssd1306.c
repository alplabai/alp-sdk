/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Solomon Systech SSD1306 monochrome OLED driver.  See header.
 */

#include <string.h>

#include "alp/chips/ssd1306.h"

/* SSD1306 I2C control byte: bit7 = continuation, bit6 = D/C#.
 * Co=0, D/C#=0 → command stream
 * Co=0, D/C#=1 → data stream */
#define CTRL_CMD_STREAM 0x00u
#define CTRL_DATA_STREAM 0x40u

/* Command opcodes used by init / control. */
#define CMD_DISPLAY_OFF 0xAEu
#define CMD_DISPLAY_ON 0xAFu
#define CMD_SET_CONTRAST 0x81u
#define CMD_NORMAL_DISPLAY 0xA6u
#define CMD_INVERT_DISPLAY 0xA7u
#define CMD_SET_MUX_RATIO 0xA8u
#define CMD_SET_DISPLAY_OFFSET 0xD3u
#define CMD_SET_START_LINE 0x40u
#define CMD_CHARGE_PUMP 0x8Du
#define CMD_MEMORY_MODE 0x20u
#define CMD_SEGMENT_REMAP 0xA1u /* column 127 mapped to SEG0 */
#define CMD_COM_SCAN_DEC 0xC8u  /* COM[N-1] → COM[0] */
#define CMD_SET_COM_PINS 0xDAu
#define CMD_SET_DCLK_DIV 0xD5u
#define CMD_SET_PRECHARGE 0xD9u
#define CMD_SET_VCOM_DETECT 0xDBu
#define CMD_DEACTIVATE_SCROLL 0x2Eu
#define CMD_SET_COLUMN_ADDR 0x21u
#define CMD_SET_PAGE_ADDR 0x22u

static alp_status_t write_cmd(ssd1306_t *dev, uint8_t cmd)
{
	uint8_t b[2] = { CTRL_CMD_STREAM, cmd };
	return alp_i2c_write(dev->bus, dev->addr, b, sizeof b);
}

static alp_status_t write_cmd2(ssd1306_t *dev, uint8_t c0, uint8_t c1)
{
	uint8_t b[3] = { CTRL_CMD_STREAM, c0, c1 };
	return alp_i2c_write(dev->bus, dev->addr, b, sizeof b);
}

static alp_status_t fb_size(const ssd1306_t *dev, size_t *out)
{
	if (dev->width == 0 || dev->height == 0) return ALP_ERR_INVAL;
	if ((dev->height % 8u) != 0u) return ALP_ERR_INVAL;
	size_t s = (size_t)dev->width * (size_t)dev->height / 8u;
	if (s > sizeof(dev->fb)) return ALP_ERR_INVAL;
	*out = s;
	return ALP_OK;
}

alp_status_t ssd1306_init(ssd1306_t *dev, alp_i2c_t *bus, uint8_t i2c_addr, uint16_t width,
                          uint16_t height)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (width > SSD1306_MAX_WIDTH) return ALP_ERR_INVAL;
	if (height > SSD1306_MAX_HEIGHT) return ALP_ERR_INVAL;
	if ((width != 128) || (height != 64 && height != 32)) {
		/* v0.1 supports 128×64 and 128×32. Other geometries land later. */
		return ALP_ERR_NOSUPPORT;
	}

	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->width       = width;
	dev->height      = height;
	dev->initialised = false;
	memset(dev->fb, 0, sizeof dev->fb);

	const uint8_t mux   = (uint8_t)(height - 1u);
	const uint8_t cpins = (height == 32u) ? 0x02u : 0x12u;

	alp_status_t  s;
	if ((s = write_cmd(dev, CMD_DISPLAY_OFF)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_DCLK_DIV, 0x80u)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_MUX_RATIO, mux)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_DISPLAY_OFFSET, 0x00u)) != ALP_OK) return s;
	if ((s = write_cmd(dev, CMD_SET_START_LINE | 0x00u)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_CHARGE_PUMP, 0x14u)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_MEMORY_MODE, 0x00u)) != ALP_OK) return s; /* horizontal */
	if ((s = write_cmd(dev, CMD_SEGMENT_REMAP)) != ALP_OK) return s;
	if ((s = write_cmd(dev, CMD_COM_SCAN_DEC)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_COM_PINS, cpins)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_CONTRAST, 0x7Fu)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_PRECHARGE, 0xF1u)) != ALP_OK) return s;
	if ((s = write_cmd2(dev, CMD_SET_VCOM_DETECT, 0x40u)) != ALP_OK) return s;
	if ((s = write_cmd(dev, 0xA4u /* DISPLAY_ALL_ON_RESUME */)) != ALP_OK) return s;
	if ((s = write_cmd(dev, CMD_NORMAL_DISPLAY)) != ALP_OK) return s;
	if ((s = write_cmd(dev, CMD_DEACTIVATE_SCROLL)) != ALP_OK) return s;
	if ((s = write_cmd(dev, CMD_DISPLAY_ON)) != ALP_OK) return s;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t ssd1306_set_display_on(ssd1306_t *dev, bool on)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return write_cmd(dev, on ? CMD_DISPLAY_ON : CMD_DISPLAY_OFF);
}

alp_status_t ssd1306_set_contrast(ssd1306_t *dev, uint8_t contrast)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return write_cmd2(dev, CMD_SET_CONTRAST, contrast);
}

alp_status_t ssd1306_set_inverted(ssd1306_t *dev, bool inverted)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return write_cmd(dev, inverted ? CMD_INVERT_DISPLAY : CMD_NORMAL_DISPLAY);
}

void ssd1306_clear(ssd1306_t *dev)
{
	if (dev == NULL) return;
	memset(dev->fb, 0, sizeof dev->fb);
}

void ssd1306_draw_pixel(ssd1306_t *dev, uint16_t x, uint16_t y, bool on)
{
	if (dev == NULL) return;
	if (x >= dev->width || y >= dev->height) return;
	const size_t idx = (size_t)(y / 8u) * (size_t)dev->width + (size_t)x;
	if (idx >= sizeof dev->fb) return;
	const uint8_t mask = (uint8_t)(1u << (y & 0x7u));
	if (on)
		dev->fb[idx] |= mask;
	else
		dev->fb[idx] = (uint8_t)(dev->fb[idx] & ~mask);
}

alp_status_t ssd1306_display(ssd1306_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	size_t       fb_bytes = 0;
	alp_status_t s        = fb_size(dev, &fb_bytes);
	if (s != ALP_OK) return s;

	const uint8_t pages = (uint8_t)(dev->height / 8u);
	const uint8_t cols  = (uint8_t)(dev->width - 1u);

	/* Address window: full screen. */
	uint8_t win_cmd[7] = { CTRL_CMD_STREAM, CMD_SET_COLUMN_ADDR,  0x00u, cols, CMD_SET_PAGE_ADDR,
		                   0x00u,           (uint8_t)(pages - 1u) };
	s                  = alp_i2c_write(dev->bus, dev->addr, win_cmd, sizeof win_cmd);
	if (s != ALP_OK) return s;

	/* Stream the framebuffer prefixed with the data control byte.
     * For v0.1 we send it in 32-byte chunks to stay friendly to
     * I2C controllers with small DMA windows. */
	enum { CHUNK = 32 };
	uint8_t buf[1 + CHUNK];
	buf[0]     = CTRL_DATA_STREAM;

	size_t off = 0;
	while (off < fb_bytes) {
		size_t n = fb_bytes - off;
		if (n > CHUNK) n = CHUNK;
		for (size_t i = 0; i < n; i++)
			buf[1 + i] = dev->fb[off + i];
		s = alp_i2c_write(dev->bus, dev->addr, buf, 1 + n);
		if (s != ALP_OK) return s;
		off += n;
	}
	return ALP_OK;
}

void ssd1306_deinit(ssd1306_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
