/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr display driver-class backend.
 *
 * ADR-0017 STATUS: Tier 1 -- pure wrapper over the upstream Zephyr
 * <zephyr/drivers/display.h> API (display_write / display_clear /
 * display_blanking_on / display_blanking_off /
 * display_get_capabilities / display_set_pixel_format).  No driver
 * code is rewritten or vendored; any panel with an upstream Zephyr
 * display driver (SSD1306, SSD1331, ILI9341, ST7789V, dummy-dc, ...)
 * resolves through devicetree.
 *
 * DT convention (mirrors the SPI / I2C / camera precedent):
 * aliases alp-display0..3.  Apps select the panel via
 * cfg->display_id; absent aliases stay NULL and open returns
 * ALP_ERR_NOT_READY.
 *
 * Registered as silicon_ref="*" at priority 50 -- always wins over
 * the zephyr_stub fallback (priority 0) on builds that link Zephyr's
 * display subsystem (CONFIG_DISPLAY); loses to future vendor-specific
 * backends (V2N DSI / parallel-RGB, Alif LCD-IF) that register at
 * priority 100 on their matching silicon.
 *
 * Coordination with the LVGL path: Zephyr's LVGL module binds the
 * DT_CHOSEN(zephyr_display) node (or a zephyr,displays compatible)
 * and owns flushing to that device -- see
 * $ZEPHYR_BASE/modules/lvgl/lvgl.c.  When CONFIG_LVGL=y, do NOT also
 * point an alp-displayN alias at the same node and open it through
 * <alp/display.h>: two writers on one panel produce interleaved
 * garbage.  Use this backend for panels the app drives directly
 * (framebuffer pushes via alp_display_blit); use lv_* for
 * LVGL-composed UIs.  alp_gui_lvgl_attach() (<alp/gui.h>, src/gui_lvgl.c)
 * makes that hand-off explicit: it creates its OWN lv_display_t bound
 * to the alp_display_t the app passes in (a DIFFERENT alias from the
 * zephyr,display chosen node LVGL's own auto-init may claim), wiring
 * LVGL's flush_cb straight to alp_display_blit() instead of going
 * through Zephyr's own lvgl_display.c glue.
 *
 * Pixel-format contract: open() keeps the driver's active format
 * when it is representable in alp_pixfmt_t, otherwise it walks the
 * panel's supported_pixel_formats and display_set_pixel_format()s
 * the first representable one (RGB565 -> RGB888 -> ARGB8888 ->
 * vertically-tiled mono).  A panel with no representable format
 * fails open with ALP_ERR_NOSUPPORT.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/23
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>

#include "display_ops.h"

#define ALP_DISPLAY_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_display, idx))), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_display, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_DISPLAY_DEV_OR_NULL(0),
	ALP_DISPLAY_DEV_OR_NULL(1),
	ALP_DISPLAY_DEV_OR_NULL(2),
	ALP_DISPLAY_DEV_OR_NULL(3),
};

/** Scratch buffer for the software clear fallback (drivers without a
 *  clear op).  Sized per-chunk, not per-frame: clear() walks the
 *  panel in scratch-sized display_write calls, so the static cost
 *  stays bounded regardless of resolution. */
#ifndef CONFIG_ALP_SDK_DISPLAY_CLEAR_CHUNK_BYTES
#define CONFIG_ALP_SDK_DISPLAY_CLEAR_CHUNK_BYTES 256
#endif

static const uint8_t _zeros[CONFIG_ALP_SDK_DISPLAY_CLEAR_CHUNK_BYTES];

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/** Map a single Zephyr display_pixel_format bit to alp_pixfmt_t.
 *  Returns true when representable.  Mono formats only qualify on
 *  vertically-tiled panels (screen_info SCREEN_INFO_MONO_VTILED) --
 *  that byte layout is what ALP_PIXFMT_MONO_VLSB promises
 *  (SSD1306-native vertical bytes). */
static bool _to_alp_pixfmt(uint32_t zfmt, uint32_t screen_info, alp_pixfmt_t *out)
{
	switch (zfmt) {
	case PIXEL_FORMAT_RGB_565:
		*out = ALP_PIXFMT_RGB565;
		return true;
	case PIXEL_FORMAT_RGB_888:
		*out = ALP_PIXFMT_RGB888;
		return true;
	case PIXEL_FORMAT_ARGB_8888:
		*out = ALP_PIXFMT_ARGB8888;
		return true;
	case PIXEL_FORMAT_MONO01:
	case PIXEL_FORMAT_MONO10:
		if ((screen_info & SCREEN_INFO_MONO_VTILED) != 0u) {
			*out = ALP_PIXFMT_MONO_VLSB;
			return true;
		}
		return false;
	default:
		return false;
	}
}

/** Preference order for normalising a panel whose active format is
 *  not representable in alp_pixfmt_t. */
static const uint32_t _fmt_prefs[] = {
	PIXEL_FORMAT_RGB_565, PIXEL_FORMAT_RGB_888, PIXEL_FORMAT_ARGB_8888,
	PIXEL_FORMAT_MONO01,  PIXEL_FORMAT_MONO10,
};

static alp_status_t z_open(const alp_display_config_t  *cfg,
                           alp_display_backend_state_t *state,
                           alp_capabilities_t          *caps_out)
{
	if (cfg->display_id >= ARRAY_SIZE(_devs)) {
		return ALP_ERR_INVAL;
	}
	const struct device *dev = _devs[cfg->display_id];
	if (dev == NULL || !device_is_ready(dev)) {
		return ALP_ERR_NOT_READY;
	}

	/* Normalise the active pixel format to one the portable
	 * surface can name.  Keep the driver's current pick when it
	 * already qualifies. */
	struct display_capabilities zcaps;
	display_get_capabilities(dev, &zcaps);
	alp_pixfmt_t mapped;
	if (!_to_alp_pixfmt(zcaps.current_pixel_format, zcaps.screen_info, &mapped)) {
		bool set_ok = false;
		for (size_t i = 0; i < ARRAY_SIZE(_fmt_prefs); ++i) {
			if ((zcaps.supported_pixel_formats & _fmt_prefs[i]) == 0u) {
				continue;
			}
			if (!_to_alp_pixfmt(_fmt_prefs[i], zcaps.screen_info, &mapped)) {
				continue;
			}
			int err = display_set_pixel_format(dev, _fmt_prefs[i]);
			if (err == 0) {
				set_ok = true;
				break;
			}
		}
		if (!set_ok) {
			return ALP_ERR_NOSUPPORT;
		}
	}

	/* Panels commonly reset blanked; turn the pixels on so the
	 * first blit is visible.  Optional driver op -- drivers
	 * without blanking report -ENOSYS / -ENOTSUP, which is fine. */
	int err = display_blanking_off(dev);
	if (err != 0 && err != -ENOSYS && err != -ENOTSUP) {
		return _errno_to_alp(err);
	}

	state->be_data  = (void *)dev;
	caps_out->flags = 0u; /* no instance-level cap flags defined for display */
	return ALP_OK;
}

static alp_status_t z_get_caps(alp_display_backend_state_t *state, alp_display_caps_t *out)
{
	const struct device *dev = (const struct device *)state->be_data;
	if (dev == NULL) {
		return ALP_ERR_NOT_READY;
	}
	struct display_capabilities zcaps;
	display_get_capabilities(dev, &zcaps);

	alp_pixfmt_t fmt;
	if (!_to_alp_pixfmt(zcaps.current_pixel_format, zcaps.screen_info, &fmt)) {
		/* open() normalised the format, so this only fires if a
		 * third party re-formatted the panel behind our back. */
		return ALP_ERR_NOSUPPORT;
	}
	out->width  = zcaps.x_resolution;
	out->height = zcaps.y_resolution;
	out->format = fmt;
	return ALP_OK;
}

static alp_status_t z_blit(alp_display_backend_state_t *state,
                           uint16_t                     x,
                           uint16_t                     y,
                           uint16_t                     w,
                           uint16_t                     h,
                           const void                  *pixels)
{
	const struct device *dev = (const struct device *)state->be_data;
	if (dev == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (w == 0u || h == 0u) {
		return ALP_ERR_INVAL;
	}

	struct display_capabilities zcaps;
	display_get_capabilities(dev, &zcaps);
	if ((uint32_t)x + w > zcaps.x_resolution || (uint32_t)y + h > zcaps.y_resolution) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	uint32_t bits_pp = DISPLAY_BITS_PER_PIXEL(zcaps.current_pixel_format);
	if (bits_pp == 0u) {
		return ALP_ERR_NOSUPPORT;
	}

	const struct display_buffer_descriptor desc = {
		.buf_size = ((uint32_t)w * h * bits_pp + 7u) / 8u,
		.width    = w,
		.height   = h,
		.pitch    = w,
	};
	return _errno_to_alp(display_write(dev, x, y, &desc, pixels));
}

static alp_status_t z_clear(alp_display_backend_state_t *state)
{
	const struct device *dev = (const struct device *)state->be_data;
	if (dev == NULL) {
		return ALP_ERR_NOT_READY;
	}

	/* Preferred path: the driver's own clear op. */
	int err = display_clear(dev);
	if (err != -ENOSYS) {
		return _errno_to_alp(err);
	}

	/* Software fallback: walk the panel with zero-filled
	 * display_write chunks.  All-zero pixel data is the portable
	 * "background": black on RGB formats, all-off bits on mono.
	 * Mono panels are written in 8-row bands so each byte column
	 * stays tile-aligned (MONO formats pack 8 vertical pixels per
	 * byte on VTILED panels). */
	struct display_capabilities zcaps;
	display_get_capabilities(dev, &zcaps);
	uint32_t bits_pp = DISPLAY_BITS_PER_PIXEL(zcaps.current_pixel_format);
	if (bits_pp == 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	uint16_t band_h = (bits_pp < 8u) ? 8u : 1u;
	if ((zcaps.y_resolution % band_h) != 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	uint32_t bytes_per_band_px = (band_h * bits_pp) / 8u; /* bytes one pixel column costs */
	uint16_t chunk_w =
	    (uint16_t)MIN((uint32_t)zcaps.x_resolution, sizeof(_zeros) / bytes_per_band_px);
	if (chunk_w == 0u) {
		return ALP_ERR_NOSUPPORT;
	}

	for (uint16_t y = 0; y < zcaps.y_resolution; y += band_h) {
		for (uint16_t x = 0; x < zcaps.x_resolution; x += chunk_w) {
			uint16_t w = (uint16_t)MIN((uint32_t)chunk_w, (uint32_t)zcaps.x_resolution - x);

			const struct display_buffer_descriptor desc = {
				.buf_size = (uint32_t)w * bytes_per_band_px,
				.width    = w,
				.height   = band_h,
				.pitch    = w,
			};
			err = display_write(dev, x, y, &desc, _zeros);
			if (err != 0) {
				return _errno_to_alp(err);
			}
		}
	}
	return ALP_OK;
}

static void z_close(alp_display_backend_state_t *state)
{
	const struct device *dev = (const struct device *)state->be_data;
	if (dev == NULL) {
		return;
	}
	/* Blank the panel on the way out -- symmetric with the
	 * blanking_off in open(); optional op, so ignore the result. */
	(void)display_blanking_on(dev);
	state->be_data = NULL;
}

static const alp_display_ops_t _ops = {
	.open     = z_open,
	.get_caps = z_get_caps,
	.blit     = z_blit,
	.clear    = z_clear,
	.close    = z_close,
};

ALP_BACKEND_REGISTER(display,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
