/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ilitek ILI9488 TFT driver.  See <alp/chips/ili9488.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ili9488.h"
#include "alp/peripheral.h"

#define ILI9488_CMD_SWRESET 0x01u
#define ILI9488_CMD_SLPOUT  0x11u
#define ILI9488_CMD_PIXFMT  0x3Au
#define ILI9488_CMD_MADCTL  0x36u
#define ILI9488_CMD_DISPON  0x29u
#define ILI9488_CMD_CASET   0x2Au
#define ILI9488_CMD_PASET   0x2Bu
#define ILI9488_CMD_RAMWR   0x2Cu

static alp_status_t ili9488_send(ili9488_t *dev, bool is_cmd, const uint8_t *buf, size_t len)
{
    alp_status_t s = alp_gpio_write(dev->dc, !is_cmd);
    if (s != ALP_OK) return s;
    return alp_spi_write(dev->bus, buf, len);
}

static alp_status_t ili9488_cmd(ili9488_t *dev, uint8_t c)
{
    return ili9488_send(dev, true, &c, 1);
}

static alp_status_t ili9488_data(ili9488_t *dev, const uint8_t *buf, size_t len)
{
    return ili9488_send(dev, false, buf, len);
}

alp_status_t ili9488_init(ili9488_t  *dev,
                          alp_spi_t  *spi,
                          alp_gpio_t *dc,
                          alp_gpio_t *reset)
{
    if (dev == NULL || spi == NULL || dc == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus   = spi;
    dev->dc    = dc;
    dev->reset = reset;

    if (reset != NULL) {
        (void)alp_gpio_write(reset, false);
        alp_delay_us(20000);
        (void)alp_gpio_write(reset, true);
        alp_delay_us(150000);
    }

    alp_status_t s = ili9488_cmd(dev, ILI9488_CMD_SWRESET);
    if (s != ALP_OK) return s;
    alp_delay_us(150000);
    s = ili9488_cmd(dev, ILI9488_CMD_SLPOUT);
    if (s != ALP_OK) return s;
    alp_delay_us(150000);

    uint8_t pixfmt = 0x55; /* 16bpp DPI + 16bpp DBI */
    s              = ili9488_cmd(dev, ILI9488_CMD_PIXFMT);
    if (s != ALP_OK) return s;
    s = ili9488_data(dev, &pixfmt, 1);
    if (s != ALP_OK) return s;

    uint8_t madctl = 0x48;
    s              = ili9488_cmd(dev, ILI9488_CMD_MADCTL);
    if (s != ALP_OK) return s;
    s = ili9488_data(dev, &madctl, 1);
    if (s != ALP_OK) return s;

    s = ili9488_cmd(dev, ILI9488_CMD_DISPON);
    if (s != ALP_OK) return s;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ili9488_set_window(ili9488_t *dev,
                                uint16_t   x0,
                                uint16_t   y0,
                                uint16_t   x1,
                                uint16_t   y1)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (x1 <= x0 || y1 <= y0) return ALP_ERR_INVAL;
    if (x1 >= ILI9488_WIDTH || y1 >= ILI9488_HEIGHT) return ALP_ERR_INVAL;

    uint8_t buf[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    };
    alp_status_t s = ili9488_cmd(dev, ILI9488_CMD_CASET);
    if (s != ALP_OK) return s;
    s = ili9488_data(dev, buf, sizeof(buf));
    if (s != ALP_OK) return s;

    buf[0] = (uint8_t)(y0 >> 8);
    buf[1] = (uint8_t)(y0 & 0xFF);
    buf[2] = (uint8_t)(y1 >> 8);
    buf[3] = (uint8_t)(y1 & 0xFF);
    s      = ili9488_cmd(dev, ILI9488_CMD_PASET);
    if (s != ALP_OK) return s;
    return ili9488_data(dev, buf, sizeof(buf));
}

alp_status_t ili9488_write_pixels(ili9488_t     *dev,
                                  const uint8_t *pixels,
                                  size_t         n_bytes)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (pixels == NULL) return ALP_ERR_INVAL;
    if ((n_bytes & 1u) != 0) return ALP_ERR_INVAL;

    alp_status_t s = ili9488_cmd(dev, ILI9488_CMD_RAMWR);
    if (s != ALP_OK) return s;
    return ili9488_data(dev, pixels, n_bytes);
}

void ili9488_deinit(ili9488_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
    dev->dc          = NULL;
    dev->reset       = NULL;
}
