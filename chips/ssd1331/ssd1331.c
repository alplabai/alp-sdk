/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Solomon Systech SSD1331 96 × 64 colour OLED driver (SPI).  See header.
 */

#include <string.h>

#include "alp/chips/ssd1331.h"

/* ------------------------------------------------------------------ */
/* Command opcodes (datasheet §9 Command Table)                        */
/* ------------------------------------------------------------------ */

#define CMD_SET_COLUMN_ADDR     0x15
#define CMD_SET_ROW_ADDR        0x75
#define CMD_SET_REMAP           0xA0
#define CMD_SET_DISPLAY_START   0xA1
#define CMD_SET_DISPLAY_OFFSET  0xA2
#define CMD_SET_NORMAL          0xA4
#define CMD_SET_MUX_RATIO       0xA8
#define CMD_SET_MASTER_CONFIG   0xAD
#define CMD_DISPLAY_OFF         0xAE
#define CMD_DISPLAY_ON          0xAF
#define CMD_SET_PRECHARGE       0xB1
#define CMD_SET_DCLK_DIV        0xB3
#define CMD_SET_PRECHARGE_LEVEL 0xBB
#define CMD_SET_VCOMH           0xBE
#define CMD_SET_MASTER_CURRENT  0x87
#define CMD_SET_CONTRAST_A      0x81
#define CMD_SET_CONTRAST_B      0x82
#define CMD_SET_CONTRAST_C      0x83

static alp_status_t write_cmds(ssd1331_t *dev, const uint8_t *cmds, size_t n) {
    /* D/C# low → command stream. */
    alp_status_t s = alp_gpio_write(dev->dc, false);
    if (s != ALP_OK) return s;
    return alp_spi_write(dev->bus, cmds, n);
}

static alp_status_t write_cmd1(ssd1331_t *dev, uint8_t c0) {
    return write_cmds(dev, &c0, 1);
}

static alp_status_t write_data(ssd1331_t *dev, const uint8_t *data, size_t n) {
    /* D/C# high → data stream. */
    alp_status_t s = alp_gpio_write(dev->dc, true);
    if (s != ALP_OK) return s;
    return alp_spi_write(dev->bus, data, n);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t ssd1331_init(ssd1331_t *dev,
                          alp_spi_t *spi,
                          alp_gpio_t *dc,
                          uint8_t *fb,
                          size_t fb_len) {
    if (dev == NULL || spi == NULL || dc == NULL || fb == NULL) {
        return ALP_ERR_INVAL;
    }
    if (fb_len < SSD1331_FB_BYTES) return ALP_ERR_INVAL;

    dev->bus    = spi;
    dev->dc     = dc;
    dev->fb     = fb;
    dev->fb_len = fb_len;
    dev->initialised = false;
    memset(dev->fb, 0, SSD1331_FB_BYTES);

    /* Datasheet recommended init sequence (§13.1).
     * Each command followed by its operand bytes is sent as a single
     * SPI write so the chip-select line stays asserted for the full
     * command. */
    static const uint8_t init_seq[] = {
        CMD_DISPLAY_OFF,
        CMD_SET_REMAP,           0x72,   /* RGB colour, 65k mode 1, normal scan. */
        CMD_SET_DISPLAY_START,   0x00,
        CMD_SET_DISPLAY_OFFSET,  0x00,
        CMD_SET_NORMAL,
        CMD_SET_MUX_RATIO,       0x3F,   /* 64MUX. */
        CMD_SET_MASTER_CONFIG,   0x8E,   /* Select external Vcc (typical). */
        0xB0, 0x0B,                      /* CMD_POWER_SAVE, disable. */
        CMD_SET_DCLK_DIV,        0xF0,
        CMD_SET_PRECHARGE,       0x32,
        CMD_SET_PRECHARGE_LEVEL, 0x3A,
        CMD_SET_VCOMH,           0x3E,
        CMD_SET_MASTER_CURRENT,  0x06,
        CMD_SET_CONTRAST_A,      0x91,
        CMD_SET_CONTRAST_B,      0x50,
        CMD_SET_CONTRAST_C,      0x7D,
        CMD_DISPLAY_ON,
    };
    alp_status_t s = write_cmds(dev, init_seq, sizeof init_seq);
    if (s != ALP_OK) return s;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ssd1331_set_display_on(ssd1331_t *dev, bool on) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return write_cmd1(dev, on ? CMD_DISPLAY_ON : CMD_DISPLAY_OFF);
}

alp_status_t ssd1331_set_master_current(ssd1331_t *dev, uint8_t current) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (current > 0x0F) return ALP_ERR_INVAL;
    const uint8_t cmd[2] = { CMD_SET_MASTER_CURRENT, current };
    return write_cmds(dev, cmd, sizeof cmd);
}

void ssd1331_clear(ssd1331_t *dev) {
    if (dev == NULL || dev->fb == NULL) return;
    memset(dev->fb, 0, SSD1331_FB_BYTES);
}

void ssd1331_draw_pixel(ssd1331_t *dev,
                        uint16_t x, uint16_t y,
                        uint16_t colour) {
    if (dev == NULL || dev->fb == NULL) return;
    if (x >= SSD1331_WIDTH || y >= SSD1331_HEIGHT) return;
    const size_t idx = ((size_t)y * SSD1331_WIDTH + (size_t)x) * SSD1331_BPP;
    if (idx + 1 >= SSD1331_FB_BYTES) return;
    /* SSD1331 expects MSB-first per the 65k colour-format spec. */
    dev->fb[idx + 0] = (uint8_t)(colour >> 8);
    dev->fb[idx + 1] = (uint8_t)(colour & 0xFFu);
}

alp_status_t ssd1331_display(ssd1331_t *dev) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

    /* Address window: full screen. */
    const uint8_t win_cmds[] = {
        CMD_SET_COLUMN_ADDR, 0x00, SSD1331_WIDTH  - 1,
        CMD_SET_ROW_ADDR,    0x00, SSD1331_HEIGHT - 1,
    };
    alp_status_t s = write_cmds(dev, win_cmds, sizeof win_cmds);
    if (s != ALP_OK) return s;
    return write_data(dev, dev->fb, SSD1331_FB_BYTES);
}

void ssd1331_deinit(ssd1331_t *dev) {
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus = NULL;
    dev->dc  = NULL;
    dev->fb  = NULL;
    dev->fb_len = 0;
}
