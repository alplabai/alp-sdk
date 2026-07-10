/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * u8g2-oled-draw -- drawing with u8g2 (https://github.com/olikraus/u8g2),
 * the monochrome-OLED graphics library almost every small embedded
 * display (SSD1306, SH1106, ST7565, ...) ends up behind.
 *
 * There's no panel wired to this board.yaml. u8g2 never actually needs
 * one to draw: every draw call (DrawStr, DrawFrame, DrawBox, ...) writes
 * into an in-RAM tile buffer; only u8g2_InitDisplay()/u8g2_SendBuffer()
 * touch real hardware, by calling out through the "device" callback you
 * hand u8g2 at setup. This example supplies u8g2_ram_null_Setup()
 * (src/u8g2_ram_null.c) -- a device that reports a 64x32 canvas and
 * otherwise does nothing -- so the SAME draw code that would target a
 * real SSD1306 runs here and gets dumped to the console as ASCII art
 * instead of pushed over I2C/SPI.
 *
 * u8g2 ships no zephyr/module.yml, so unlike most `libraries:` entries
 * in this SDK, a hand-picked minimal subset of u8g2's csrc/ tree is
 * vendored in-tree instead of west-fetched (see zephyr/CMakeLists.txt's
 * comment block for which files and why). board.yaml's
 * `libraries: [u8g2]` sets CONFIG_ALP_SDK_U8G2_VENDORED_CORE, the
 * selection-scoped Kconfig knob that gates compiling that vendored
 * subset into the build -- this example's own CMakeLists.txt only
 * lists its own src/ C sources.
 *
 * What success looks like: an ASCII-art rectangle+box+string, then
 * `[u8g2-oled-draw] done`.
 */

#include <stdio.h>

#include "u8g2.h"
#include "u8g2_ram_null.h"

int main(void)
{
	/* u8g2_t is one struct for the whole library: geometry, the RAM
	 * tile buffer pointer, the current font, draw color, clip window
	 * -- everything. There's no heap allocation anywhere in this
	 * example; u8g2_ram_null_Setup() points u8g2 at a `static` buffer
	 * sized for the 64x32 canvas (see u8g2_ram_null.h). */
	u8g2_t u8g2;

	u8g2_ram_null_Setup(&u8g2);

	/* Real firmware would call u8g2_InitDisplay(&u8g2) here to push
	 * the panel's power-on init sequence over the bus. Our device
	 * callback treats that as a no-op (see u8g2_ram_null_cb), so
	 * skipping the call changes nothing observable -- it's included
	 * only as the line you'd UN-comment when swapping in real
	 * hardware (see README.md "HW swap"). */
	/* u8g2_InitDisplay(&u8g2); */

	/* u8g2_ClearBuffer just memsets the tile buffer to 0 -- draw
	 * calls OR bits in, they never assume a pristine buffer. Skipping
	 * this on a freshly `static`-zeroed buffer would be harmless
	 * here, but real firmware re-enters main-loop redraws on an
	 * already-drawn buffer, so always clear first. */
	u8g2_ClearBuffer(&u8g2);

	/* Frame: 1px outline rectangle. u8g2's coordinate origin is
	 * top-left, x right, y down -- same convention as most 2D
	 * graphics APIs (and the opposite of math-graph y-up). Inset by
	 * 1px so the frame doesn't clip against the canvas edge. */
	u8g2_DrawFrame(&u8g2, 1, 1, U8G2_RAM_NULL_WIDTH_PX - 2, U8G2_RAM_NULL_HEIGHT_PX - 2);

	/* Filled box in a corner, distinct from the frame so the ASCII
	 * dump shows both primitives at once. */
	u8g2_DrawBox(&u8g2, 4, 4, 10, 8);

	/* Fonts are separately-linked glyph tables (u8g2_font_6x10_tr is
	 * defined in src/u8g2_font_6x10_tr.c, NOT the vendored
	 * u8g2_fonts.c -- see that file's header comment for why). "tr"
	 * suffix = Transparent glyph background, reduced/restricted
	 * charset (ASCII 32-127).
	 * DrawStr's (x, y) is the text BASELINE, not the top-left glyph
	 * corner -- y=28 keeps a 10px-tall font's descenders inside the
	 * 32px canvas. */
	u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
	u8g2_DrawStr(&u8g2, 18, 28, "alp");

	/* Real firmware would call u8g2_SendBuffer(&u8g2) here to push
	 * the tile buffer to the panel over I2C/SPI. We read the RAM
	 * buffer directly instead -- see u8g2_ram_null_dump() for the
	 * byte encoding. */
	printf("[u8g2-oled-draw] %ux%u canvas (# = lit pixel):\n",
	       (unsigned)U8G2_RAM_NULL_WIDTH_PX,
	       (unsigned)U8G2_RAM_NULL_HEIGHT_PX);
	u8g2_ram_null_dump(&u8g2);

	printf("[u8g2-oled-draw] done\n");
	return 0;
}
