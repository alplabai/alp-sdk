/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1121 round-2 follow-up: tests/unit/display_cdc200_bounds proves
 * cdc200_validate_transfer() itself is correct, but a mutation that deletes
 * the *call* to it inside display_cdc200.c's four entry points (proven in
 * review: all four calls removed, 3/3 twister configurations still PASS)
 * is invisible to a header-only test. This suite links the real
 * display_cdc200.c and drives cdc200_generic_write/_read and
 * cdc200_display_write/_read -- the actual call sites -- with a hand-built
 * `struct device` (no devicetree instance needed; see this suite's
 * CMakeLists.txt for why the driver builds unmodified under native_sim).
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/ztest.h>

#include "display_cdc200.h"

/* cdc200_generic_write/_read implement Zephyr's generic
 * display_write_t/display_read_t API (registered via
 * `struct display_driver_api` in display_cdc200.c) and so have no
 * standalone prototype in display_cdc200.h -- declare them here to call
 * them directly, matching the definitions in display_cdc200.c exactly.
 */
int cdc200_generic_write(const struct device                    *dev,
                         const uint16_t                          x,
                         const uint16_t                          y,
                         const struct display_buffer_descriptor *desc,
                         const void                             *buf);
int cdc200_generic_read(const struct device                    *dev,
                        const uint16_t                          x,
                        const uint16_t                          y,
                        const struct display_buffer_descriptor *desc,
                        void                                   *buf);

ZTEST_SUITE(display_cdc200_entrypoints, NULL, NULL, NULL, NULL, NULL);

/* A small 16x16 ARGB8888 layer window -- big enough to need a real
 * multi-row memcpy loop, small enough to keep the fixtures readable.
 */
#define LAYER_W  16
#define LAYER_H  16
#define PIX_SIZE 4u
#define FB_BYTES (LAYER_W * LAYER_H * PIX_SIZE)

static uint8_t fb_layer1[FB_BYTES];

static void make_dev(struct device *dev, struct cdc200_config *config, struct cdc200_data *data)
{
	memset(dev, 0, sizeof(*dev));
	memset(config, 0, sizeof(*config));
	memset(data, 0, sizeof(*data));

	config->layer[CDC_LAYER_1] = (struct cdc200_layer_config){
		.x0         = 0,
		.y0         = 0,
		.x1         = LAYER_W,
		.y1         = LAYER_H,
		.pixel_size = PIX_SIZE,
	};

	memset(fb_layer1, 0, sizeof(fb_layer1));
	data->curr_fb[CDC_LAYER_1] = fb_layer1;

	dev->config = config;
	dev->data   = data;
}

/* A 4x4 ARGB8888 source/dest pattern, distinguishable from the framebuffer's
 * zero-initialised background so a copy (or its absence) is provable.
 */
#define RECT_W     4
#define RECT_H     4
#define RECT_BYTES (RECT_W * RECT_H * PIX_SIZE)

ZTEST(display_cdc200_entrypoints, test_generic_write_valid_rect_copies_into_framebuffer)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              src[RECT_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(src, 0xAB, sizeof(src));

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(src),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_generic_write(&dev, 0, 0, &desc, src);

	zassert_equal(ret, 0, "valid in-window write must succeed, got %d", ret);
	/* First row of the framebuffer must now hold the source pattern. */
	zassert_mem_equal(fb_layer1,
	                  src,
	                  RECT_W * PIX_SIZE,
	                  "valid write must actually copy pixel data into the framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_generic_write_rect_past_window_rejected_untouched)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              src[RECT_BYTES];
	uint8_t              fb_before[FB_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(src, 0xCD, sizeof(src));
	memcpy(fb_before, fb_layer1, sizeof(fb_before));

	/* x = LAYER_W - 1 with a 4-wide rect overruns the 16-wide window by 3
	 * columns -- exactly the #1121 defect class (rectangle extends past
	 * the layer's window).
	 */
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(src),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_generic_write(&dev, LAYER_W - 1, 0, &desc, src);

	zassert_equal(ret, -EINVAL, "out-of-window write must be rejected, got %d", ret);
	zassert_mem_equal(fb_layer1,
	                  fb_before,
	                  sizeof(fb_before),
	                  "a rejected write must never touch the framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_generic_write_undersized_buf_size_rejected)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              src[RECT_BYTES];
	uint8_t              fb_before[FB_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(src, 0xEF, sizeof(src));
	memcpy(fb_before, fb_layer1, sizeof(fb_before));

	/* desc claims a 4x4 transfer but buf_size is one byte short of that
	 * -- the #1121 "undersized caller buffer" defect class.
	 */
	struct display_buffer_descriptor desc = {
		.buf_size = RECT_BYTES - 1,
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_generic_write(&dev, 0, 0, &desc, src);

	zassert_equal(ret, -EINVAL, "undersized buf_size must be rejected, got %d", ret);
	zassert_mem_equal(fb_layer1,
	                  fb_before,
	                  sizeof(fb_before),
	                  "a rejected write must never touch the framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_generic_write_pitch_narrower_than_width_rejected)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              src[RECT_BYTES];
	uint8_t              fb_before[FB_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(src, 0x12, sizeof(src));
	memcpy(fb_before, fb_layer1, sizeof(fb_before));

	/* pitch < width -- the #1121 "pitch narrower than claimed width"
	 * defect class.
	 */
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(src),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W - 1,
	};

	ret = cdc200_generic_write(&dev, 0, 0, &desc, src);

	zassert_equal(ret, -EINVAL, "pitch < width must be rejected, got %d", ret);
	zassert_mem_equal(fb_layer1,
	                  fb_before,
	                  sizeof(fb_before),
	                  "a rejected write must never touch the framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_generic_read_valid_rect_copies_out)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              dst[RECT_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(fb_layer1, 0x77, RECT_W * PIX_SIZE);
	memset(dst, 0, sizeof(dst));

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(dst),
		.width    = RECT_W,
		.height   = 1,
		.pitch    = RECT_W,
	};

	ret = cdc200_generic_read(&dev, 0, 0, &desc, dst);

	zassert_equal(ret, 0, "valid in-window read must succeed, got %d", ret);
	zassert_mem_equal(dst,
	                  fb_layer1,
	                  RECT_W * PIX_SIZE,
	                  "valid read must actually copy pixel data out of the framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_generic_read_rect_past_window_rejected_untouched)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              dst[RECT_BYTES];
	uint8_t              dst_before[RECT_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(dst, 0x55, sizeof(dst));
	memcpy(dst_before, dst, sizeof(dst_before));

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(dst),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	/* y = LAYER_H - 1 with a 4-tall rect overruns the 16-tall window. */
	ret = cdc200_generic_read(&dev, 0, LAYER_H - 1, &desc, dst);

	zassert_equal(ret, -EINVAL, "out-of-window read must be rejected, got %d", ret);
	zassert_mem_equal(dst,
	                  dst_before,
	                  sizeof(dst_before),
	                  "a rejected read must never touch the caller's buffer");
}

ZTEST(display_cdc200_entrypoints, test_display_write_invalid_layer_index_rejected)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              src[RECT_BYTES] = { 0 };
	int                  ret;

	make_dev(&dev, &config, &data);

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(src),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_display_write(&dev, CDC_LAYER_MAX, 0, 0, &desc, src);

	zassert_equal(ret, -EINVAL, "an out-of-range layer index must be rejected, got %d", ret);
}

ZTEST(display_cdc200_entrypoints, test_display_write_valid_layer2_rect_copies_into_framebuffer)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	static uint8_t       fb_layer2[FB_BYTES];
	uint8_t              src[RECT_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	config.layer[CDC_LAYER_2] = (struct cdc200_layer_config){
		.x0         = 0,
		.y0         = 0,
		.x1         = LAYER_W,
		.y1         = LAYER_H,
		.pixel_size = PIX_SIZE,
	};
	memset(fb_layer2, 0, sizeof(fb_layer2));
	data.curr_fb[CDC_LAYER_2] = fb_layer2;
	memset(src, 0x9A, sizeof(src));

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(src),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_display_write(&dev, CDC_LAYER_2, 0, 0, &desc, src);

	zassert_equal(ret, 0, "valid layer-2 write must succeed, got %d", ret);
	zassert_mem_equal(fb_layer2,
	                  src,
	                  RECT_W * PIX_SIZE,
	                  "valid layer-2 write must copy pixel data into layer 2's framebuffer");
}

ZTEST(display_cdc200_entrypoints, test_display_read_rect_past_window_rejected_untouched)
{
	struct device        dev;
	struct cdc200_config config;
	struct cdc200_data   data;
	uint8_t              dst[RECT_BYTES];
	uint8_t              dst_before[RECT_BYTES];
	int                  ret;

	make_dev(&dev, &config, &data);
	memset(dst, 0x66, sizeof(dst));
	memcpy(dst_before, dst, sizeof(dst_before));

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(dst),
		.width    = RECT_W,
		.height   = RECT_H,
		.pitch    = RECT_W,
	};

	ret = cdc200_display_read(&dev, CDC_LAYER_1, LAYER_W - 1, 0, &desc, dst);

	zassert_equal(ret, -EINVAL, "out-of-window read must be rejected, got %d", ret);
	zassert_mem_equal(dst,
	                  dst_before,
	                  sizeof(dst_before),
	                  "a rejected read must never touch the caller's buffer");
}
