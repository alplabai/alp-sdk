/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display chip smokes: ssd1306, ssd1331, st7789, ili9341, ili9488,
 * ra8875, sh1106, il3820, gdew0154t8 -- lifecycle, NULL-arg validation,
 * and pure framebuffer-math coverage.  The fake-backed ssd1306 tests
 * exercise the real transfer path against the fake i2c-emul target in
 * fake_ssd1306.c.
 */

#include <zephyr/ztest.h>

#include "alp/chips/gdew0154t8.h"
#include "alp/chips/il3820.h"
#include "alp/chips/ili9341.h"
#include "alp/chips/ili9488.h"
#include "alp/chips/ra8875.h"
#include "alp/chips/sh1106.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/st7789.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

#include "fakes.h"

/* ------------------------------------------------------------------ */
/* ssd1306                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_init_invalid_geometry)
{
	ssd1306_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 96, 48),
	              ALP_ERR_NOSUPPORT,
	              "v0.1 supports only 128x64 or 128x32");
	zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 0, 0),
	              ALP_ERR_NOSUPPORT,
	              "zero-sized geometry must be invalid");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ssd1306_clear_and_pixel_safe_without_init)
{
	ssd1306_t dev = { 0 };
	/* These two functions must be NULL-safe and tolerate an
     * uninitialised context — they only touch the in-memory
     * framebuffer, not the panel. */
	ssd1306_clear(&dev);
	ssd1306_draw_pixel(&dev, 1000, 1000, true); /* OOB silently ignored */
	ssd1306_clear(NULL);                        /* NULL is a no-op */
	ssd1306_draw_pixel(NULL, 0, 0, true);
}

ZTEST(alp_chips, test_ssd1306_display_rejects_uninitialised)
{
	ssd1306_t dev = { 0 };
	zassert_equal(ssd1306_display(&dev),
	              ALP_ERR_NOT_READY,
	              "display() on uninitialised driver must be NOT_READY");
}

/* ------------------------------------------------------------------ */
/* ssd1306 framebuffer logic                                           */
/*                                                                     */
/* These tests exercise the pure pixel-buffer manipulation path —      */
/* no I2C transfers, no emulator fixture.  The framebuffer is part     */
/* of the public struct, so we can drive draw_pixel / clear directly   */
/* and inspect the bytes the panel would receive via display().        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_draw_pixel_sets_correct_bit)
{
	/* Geometry filled in manually so init's I2C side-effects don't
     * matter — we're testing framebuffer math. */
	ssd1306_t dev = { .width = 128, .height = 64 };

	ssd1306_draw_pixel(&dev, 0, 0, true);
	/* y=0 → page 0, bit 0 of column 0 → fb[0] bit 0 */
	zassert_equal(dev.fb[0], 0x01u, "got 0x%02x", dev.fb[0]);

	ssd1306_draw_pixel(&dev, 0, 7, true);
	/* y=7 → still page 0, bit 7 of column 0 → fb[0] bit 7 */
	zassert_equal(dev.fb[0], 0x81u, "got 0x%02x", dev.fb[0]);

	ssd1306_draw_pixel(&dev, 0, 8, true);
	/* y=8 → page 1, bit 0 of column 0 → fb[width] bit 0 */
	zassert_equal(dev.fb[128], 0x01u, "got 0x%02x", dev.fb[128]);

	ssd1306_draw_pixel(&dev, 127, 63, true);
	/* y=63 → page 7, bit 7 of column 127 → fb[7*128 + 127] bit 7 */
	zassert_equal(dev.fb[7 * 128 + 127], 0x80u);
}

ZTEST(alp_chips, test_ssd1306_draw_pixel_clears_bit)
{
	ssd1306_t dev = { .width = 128, .height = 64 };
	ssd1306_draw_pixel(&dev, 5, 3, true);
	zassert_equal(dev.fb[5], 0x08u, "set first");

	ssd1306_draw_pixel(&dev, 5, 3, false);
	zassert_equal(dev.fb[5], 0x00u, "clear should mask the bit");
}

ZTEST(alp_chips, test_ssd1306_draw_pixel_oob_silently_ignored)
{
	ssd1306_t dev = { .width = 128, .height = 64 };
	/* These must not write outside the framebuffer. */
	ssd1306_draw_pixel(&dev, 128, 0, true);     /* x at width */
	ssd1306_draw_pixel(&dev, 0, 64, true);      /* y at height */
	ssd1306_draw_pixel(&dev, 9999, 9999, true); /* far OOB */

	for (size_t i = 0; i < sizeof dev.fb; i++) {
		zassert_equal(
		    dev.fb[i], 0u, "fb[%zu] = 0x%02x; OOB writes should be ignored", i, dev.fb[i]);
	}
}

ZTEST(alp_chips, test_ssd1306_clear_wipes_only_fb)
{
	ssd1306_t dev = { .width = 128, .height = 64, .addr = 0x3C };
	for (size_t i = 0; i < sizeof dev.fb; i++)
		dev.fb[i] = 0xAA;

	ssd1306_clear(&dev);

	for (size_t i = 0; i < sizeof dev.fb; i++) {
		zassert_equal(dev.fb[i], 0u, "fb[%zu] not cleared", i);
	}
	/* Other fields preserved. */
	zassert_equal(dev.width, 128u);
	zassert_equal(dev.height, 64u);
	zassert_equal(dev.addr, 0x3Cu);
}

/* ------------------------------------------------------------------ */
/* ssd1331 (v0.2 chip — SPI)                                           */
/*                                                                     */
/* The SSD1331 framebuffer is 12 KiB; we keep it in BSS to avoid       */
/* putting it on the ztest thread stack.                                */
/* ------------------------------------------------------------------ */

static uint8_t ssd1331_test_fb[SSD1331_FB_BYTES];

ZTEST(alp_chips, test_ssd1331_init_null_args)
{
	ssd1331_t dev;
	zassert_equal(ssd1331_init(NULL, NULL, NULL, ssd1331_test_fb, sizeof ssd1331_test_fb),
	              ALP_ERR_INVAL);
	/* Even non-NULL ctx + NULL spi/dc/fb → INVAL. */
	zassert_equal(ssd1331_init(&dev, NULL, NULL, ssd1331_test_fb, sizeof ssd1331_test_fb),
	              ALP_ERR_INVAL);
	/* NULL framebuffer → INVAL. */
	zassert_equal(ssd1331_init(&dev, NULL, NULL, NULL, sizeof ssd1331_test_fb), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ssd1331_pixel_safe_without_init)
{
	ssd1331_t dev = { 0 };
	dev.fb        = ssd1331_test_fb;
	dev.fb_len    = sizeof ssd1331_test_fb;

	/* clear/draw_pixel touch only the in-memory framebuffer — safe
     * pre-init.  Clear first so any stale bits from earlier cases
     * are wiped. */
	ssd1331_clear(&dev);
	ssd1331_draw_pixel(&dev, 1000, 1000, 0xFFFFu); /* OOB silently ignored. */
	ssd1331_draw_pixel(NULL, 0, 0, 0u);            /* NULL ctx is a no-op. */
	ssd1331_clear(NULL);

	/* In-bounds pixel writes the right RGB565 bytes (MSB-first). */
	ssd1331_draw_pixel(&dev, 0, 0, 0xF800u); /* red */
	zassert_equal(ssd1331_test_fb[0], 0xF8u);
	zassert_equal(ssd1331_test_fb[1], 0x00u);
}

ZTEST(alp_chips, test_ssd1331_display_rejects_uninitialised)
{
	ssd1331_t dev = { 0 };
	zassert_equal(ssd1331_display(&dev), ALP_ERR_NOT_READY);
	zassert_equal(ssd1331_set_display_on(&dev, true), ALP_ERR_NOT_READY);
	zassert_equal(ssd1331_set_master_current(&dev, 0x06), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_ssd1331_rgb565_helper)
{
	/* (0xFF, 0x00, 0x00) → 0xF800 (max red).
     * (0x00, 0xFF, 0x00) → 0x07E0 (max green).
     * (0x00, 0x00, 0xFF) → 0x001F (max blue). */
	zassert_equal(ssd1331_rgb565(0xFF, 0x00, 0x00), 0xF800u);
	zassert_equal(ssd1331_rgb565(0x00, 0xFF, 0x00), 0x07E0u);
	zassert_equal(ssd1331_rgb565(0x00, 0x00, 0xFF), 0x001Fu);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.AI batch -- displays                                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_st7789_init_null_args)
{
	st7789_t dev;
	/* NULL ctx / NULL spi / NULL dc -- INVAL.  Real bus opens are
     * not required at this layer; the driver rejects NULL handles
     * before touching them. */
	zassert_equal(st7789_init(NULL, NULL, NULL, NULL, 240, 320), ALP_ERR_INVAL);
	zassert_equal(st7789_init(&dev, NULL, NULL, NULL, 240, 320), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ili9341_init_null_args)
{
	ili9341_t dev;
	zassert_equal(ili9341_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ili9341_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ili9488_init_null_args)
{
	ili9488_t dev;
	zassert_equal(ili9488_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ili9488_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ra8875_init_null_args)
{
	ra8875_t dev;
	zassert_equal(ra8875_init(NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ra8875_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_sh1106_init_null_args)
{
	sh1106_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(sh1106_init(NULL, bus, SH1106_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(sh1106_init(&dev, NULL, SH1106_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(sh1106_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_sh1106_draw_pixel_clips_oob)
{
	sh1106_t dev = { 0 };
	/* Pre-init draw_pixel is silently no-op (no crash, no fb write). */
	sh1106_draw_pixel(&dev, SH1106_WIDTH + 5, 0, true);
	sh1106_draw_pixel(&dev, 0, SH1106_HEIGHT + 5, true);
	sh1106_draw_pixel(NULL, 10, 10, true);
	for (size_t i = 0; i < sizeof(dev.fb); i++) {
		zassert_equal(
		    dev.fb[i], 0u, "fb[%zu] = 0x%02x; OOB write must not corrupt fb", i, dev.fb[i]);
	}
}

ZTEST(alp_chips, test_il3820_init_null_args)
{
	il3820_t dev;
	zassert_equal(il3820_init(NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(il3820_init(&dev, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gdew0154t8_init_null_args)
{
	gdew0154t8_t dev;
	zassert_equal(gdew0154t8_init(NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(gdew0154t8_init(&dev, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

/* ==================================================================== */
/* Register-protocol tests (fake i2c-emul target)                        */
/* ==================================================================== */
/* Compiled out by maintainer decision (commit 4b0b33d5): chip          */
/* register-protocol validation happens on real silicon via             */
/* examples/v2n/v2n-brd-i2c-bringup, not against these i2c-emul doubles  */
/* (see CHANGELOG.md, "Removed -- chips: fake-based register tests").    */
/* Gated on DT_NODE_EXISTS(DT_NODELABEL(fake_*)) so they compile to      */
/* no-ops while the overlay's fake_* nodes stay commented out.          */
/* ==================================================================== */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso))

ZTEST(alp_chips, test_fake_ssd1306_init_streams_documented_opcodes)
{
	fake_ssd1306_reset_logs();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	ssd1306_t dev;
	zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 128, 64),
	              ALP_OK,
	              "fake records command bytes — init must succeed");

	/* The init sequence MUST contain DISPLAY_OFF (0xAE), CHARGE_PUMP
     * enable (0x8D, 0x14), MEMORY_MODE horizontal (0x20, 0x00), and
     * DISPLAY_ON (0xAF) in order.  The fake's command log captures
     * every byte after the 0x00 control byte, so we scan it for the
     * documented anchors. */
	const uint8_t *log = fake_ssd1306_cmd_log();
	const size_t   len = fake_ssd1306_cmd_log_len();
	zassert_true(len > 0, "init must stream at least one command");

	/* DISPLAY_OFF must come first per the datasheet's recommended
     * power-up sequence. */
	zassert_equal(log[0], 0xAEu, "first opcode must be DISPLAY_OFF");

	/* DISPLAY_ON must come last. */
	zassert_equal(log[len - 1], 0xAFu, "last opcode must be DISPLAY_ON");

	/* Charge pump enable must appear: 0x8D followed by 0x14. */
	bool found_charge_pump = false;
	for (size_t i = 0; i + 1 < len; i++) {
		if (log[i] == 0x8Du && log[i + 1] == 0x14u) {
			found_charge_pump = true;
			break;
		}
	}
	zassert_true(found_charge_pump, "init must enable the charge pump (0x8D 0x14)");

	/* No data bytes streamed during init (no fb push). */
	zassert_equal(fake_ssd1306_data_log_len(), 0u, "init must not push any framebuffer bytes");

	ssd1306_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_ssd1306_display_pushes_full_framebuffer)
{
	fake_ssd1306_reset_logs();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	ssd1306_t dev;
	zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 128, 64), ALP_OK);

	fake_ssd1306_reset_logs(); /* drop the init bytes from the logs. */

	/* Write a single test pixel and flush. */
	ssd1306_clear(&dev);
	ssd1306_draw_pixel(&dev, 0, 0, true);
	zassert_equal(ssd1306_display(&dev), ALP_OK);

	/* display() pushes width*height/8 = 1024 framebuffer bytes. */
	zassert_equal(fake_ssd1306_data_log_len(),
	              1024u,
	              "expected 1024 fb bytes, got %zu",
	              fake_ssd1306_data_log_len());

	/* The first byte (page=0, col=0) should hold our pixel. */
	zassert_equal(fake_ssd1306_data_log()[0], 0x01u, "page=0, col=0, bit 0 should be set");

	/* The address-window command must precede the data — confirm
     * the column/page-range opcodes appear in the cmd_log. */
	const uint8_t *cmd             = fake_ssd1306_cmd_log();
	const size_t   clen            = fake_ssd1306_cmd_log_len();
	bool           found_col_addr  = false;
	bool           found_page_addr = false;
	for (size_t i = 0; i < clen; i++) {
		if (cmd[i] == 0x21u) found_col_addr = true;
		if (cmd[i] == 0x22u) found_page_addr = true;
	}
	zassert_true(found_col_addr, "display must set column address (0x21)");
	zassert_true(found_page_addr, "display must set page address (0x22)");

	ssd1306_deinit(&dev);
	alp_i2c_close(bus);
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso)) — fake-emul block */
