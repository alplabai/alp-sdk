/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * See u8g2_ram_null.h for the "why".
 */

#include "u8g2_ram_null.h"

#include <stdio.h>

/* u8x8_display_info_t geometry for the null canvas. Every field below
 * that isn't tile/pixel size is a bus timing parameter -- meaningless
 * here since u8g2_ram_null_cb() never talks to a bus, so they're all
 * zeroed. Real panel drivers (see u8x8_d_ssd1306_64x32.c) fill these
 * in from the datasheet; that's the only thing a HW swap changes. */
static const u8x8_display_info_t u8g2_ram_null_info = {
	.chip_enable_level        = 0,
	.chip_disable_level       = 1,
	.post_chip_enable_wait_ns = 0,
	.pre_chip_disable_wait_ns = 0,
	.reset_pulse_width_ms     = 0,
	.post_reset_wait_ms       = 0,
	.sda_setup_time_ns        = 0,
	.sck_pulse_width_ns       = 0,
	.sck_clock_hz             = 400000UL,
	.spi_mode                 = 0,
	.i2c_bus_clock_100kHz     = 4,
	.data_setup_time_ns       = 0,
	.write_pulse_width_ns     = 0,
	.tile_width               = U8G2_RAM_NULL_WIDTH_PX / 8,
	.tile_height              = U8G2_RAM_NULL_HEIGHT_PX / 8,
	.default_x_offset         = 0,
	.flipmode_x_offset        = 0,
	.pixel_width              = U8G2_RAM_NULL_WIDTH_PX,
	.pixel_height             = U8G2_RAM_NULL_HEIGHT_PX,
};

/* The device callback. Only U8X8_MSG_DISPLAY_SETUP_MEMORY is handled --
 * that's the one message u8g2_SetupDisplay() dispatches synchronously
 * (via u8x8_SetupMemory(), called from u8x8_Setup()) to learn the
 * panel's geometry before u8g2_SetupBuffer() sizes anything against it.
 * Every other message (INIT, DRAW_TILE, SET_POWER_SAVE, ...) is real
 * panel I/O that main.c never triggers (no InitDisplay()/SendBuffer()
 * call), so "return 1 and do nothing" for them is never even reached in
 * this example -- it's here so the callback is a complete, correct u8g2
 * device by the API contract, not just enough to pass this one test. */
static uint8_t u8g2_ram_null_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
	(void)arg_int;
	(void)arg_ptr;

	switch (msg) {
	case U8X8_MSG_DISPLAY_SETUP_MEMORY:
		u8x8_d_helper_display_setup_memory(u8x8, &u8g2_ram_null_info);
		break;
	default:
		break;
	}
	return 1;
}

void u8g2_ram_null_Setup(u8g2_t *u8g2)
{
	/* Full-buffer mode: one tile-row-band per 8 pixel rows, sized for
	 * the WHOLE canvas (as opposed to "page" mode, which buffers only
	 * a few bands at a time to save RAM on real MCUs -- irrelevant
	 * here since the "device" costs nothing to hold in full). */
	static uint8_t buf[U8G2_RAM_NULL_WIDTH_PX * (U8G2_RAM_NULL_HEIGHT_PX / 8)];

	u8g2_SetupDisplay(u8g2, u8g2_ram_null_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
	u8g2_SetupBuffer(
	    u8g2, buf, U8G2_RAM_NULL_HEIGHT_PX / 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
}

void u8g2_ram_null_dump(u8g2_t *u8g2)
{
	const uint8_t *buf    = u8g2_GetBufferPtr(u8g2);
	u8g2_uint_t    width  = u8g2_GetDisplayWidth(u8g2);
	u8g2_uint_t    height = u8g2_GetDisplayHeight(u8g2);

	/* u8g2_ll_hvline_vertical_top_lsb's encoding (see u8g2_ll_hvline.c):
	 * the buffer is a sequence of 8-row bands; within a band, byte[x]
	 * holds that column's 8 pixels with bit 0 = the band's top row. */
	for (u8g2_uint_t y = 0; y < height; y++) {
		for (u8g2_uint_t x = 0; x < width; x++) {
			uint8_t byte  = buf[(y / 8) * width + x];
			uint8_t pixel = (byte >> (y % 8)) & 1u;

			putchar(pixel ? '#' : '.');
		}
		putchar('\n');
	}
}
