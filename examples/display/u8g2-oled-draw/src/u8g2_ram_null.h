/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * u8g2_ram_null -- a u8g2 "device" that never touches hardware.
 *
 * u8g2 draws into a RAM tile buffer regardless of which panel it's
 * pointed at; the device callback (u8x8_msg_cb) only matters when u8g2
 * pushes that buffer OUT (u8g2_InitDisplay / u8g2_SendBuffer). This
 * device implements just enough of the callback contract to report a
 * display geometry -- U8X8_MSG_DISPLAY_SETUP_MEMORY -- and otherwise
 * does nothing. main.c never calls InitDisplay/SendBuffer, so the
 * "nothing" branches are never exercised; the RAM buffer is read
 * directly instead (see u8g2_ram_null_dump()).
 */

#ifndef U8G2_RAM_NULL_H_
#define U8G2_RAM_NULL_H_

#include "u8g2.h"

/* Canvas size: matches the u8x8_d_ssd1306_64x32.c real-panel driver
 * (see README.md's HW-swap note) so the drawing code in main.c is
 * unchanged when this null device is swapped for the real one. */
#define U8G2_RAM_NULL_WIDTH_PX  64u
#define U8G2_RAM_NULL_HEIGHT_PX 32u

/**
 * @brief Wire up a u8g2_t against the in-RAM null device.
 * @param[out] u8g2 Uninitialised u8g2 instance to configure.
 *
 * Equivalent in spirit to the u8g2_Setup_<panel>_f() functions
 * u8g2_d_setup.c generates per real panel -- same u8g2_SetupDisplay() +
 * u8g2_SetupBuffer() shape, just pointed at u8g2_ram_null_cb() instead
 * of a chip driver. No I/O happens; safe to call with no bus configured.
 */
void u8g2_ram_null_Setup(u8g2_t *u8g2);

/**
 * @brief Dump the u8g2 RAM tile buffer as ASCII art.
 * @param[in] u8g2 A u8g2 instance previously set up via
 *                  u8g2_ram_null_Setup() (any other device's tile-buffer
 *                  layout may not match -- see the .c file for the byte
 *                  encoding this reads).
 *
 * Prints one line per pixel row, '#' for a set pixel and '.' for clear.
 * Reads u8g2_GetBufferPtr() directly -- there is no "get pixel" call in
 * u8g2's public API, only draw calls, so a host-side dump has to know
 * the vertical-LSB tile encoding u8g2_ll_hvline_vertical_top_lsb() uses.
 */
void u8g2_ram_null_dump(u8g2_t *u8g2);

#endif /* U8G2_RAM_NULL_H_ */
