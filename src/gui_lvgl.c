/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/gui.h> -- alp_gui_lvgl_attach() LVGL bridge (issue #23).
 *
 * Two bodies compiled from the same TU, split on ALP_HAS_LVGL (set by
 * the build whenever the app links upstream LVGL -- see <alp/gui.h>):
 *
 *   ALP_HAS_LVGL defined:     the real LVGL v9 hand-off below.  Creates
 *                             an lv_display_t sized to the alp_display_t's
 *                             reported geometry, binds a flush_cb that
 *                             forwards each dirty rect to
 *                             alp_display_blit(), and hands back ALP_OK.
 *   ALP_HAS_LVGL undefined:   the original guard-clause degrade (NULL ->
 *                             INVAL, else -> NOSUPPORT) -- kept verbatim
 *                             so the symbol always links even when no
 *                             build wires LVGL in (plain-CMake default,
 *                             Zephyr builds without CONFIG_LVGL).
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/23
 */

#include <stddef.h>

#include "alp/gui.h"

#ifdef ALP_HAS_LVGL

#include <stdbool.h>
#include <stdlib.h>

/* Partial-refresh draw-buffer height, in display lines.  A Kconfig on
 * Zephyr (zephyr/kconfigs/hw-info.kconfig); a plain #define default
 * everywhere else (plain-CMake / Yocto have no Kconfig).  16 lines is a
 * conservative middle ground -- big enough that a 320px-wide RGB565
 * panel amortises LVGL's per-flush overhead, small enough (<= 10 KB at
 * 320 * 16 * 2 bytes) to not dent a constrained heap. */
#ifndef CONFIG_ALP_GUI_LVGL_BUF_LINES
#define CONFIG_ALP_GUI_LVGL_BUF_LINES 16
#endif

/**
 * @brief Map an alp_pixfmt_t to its LVGL v9 lv_color_format_t + bpp.
 *
 * Only the alp formats LVGL v9 can represent byte-for-byte are mapped;
 * ALP_PIXFMT_MONO_VLSB has no match -- LVGL's closest analogue,
 * LV_COLOR_FORMAT_I1, packs 8 pixels per byte MSB-first *horizontally*,
 * not the SSD1306-style vertical-byte packing alp_pixfmt_t's MONO_VLSB
 * documents, so mapping it would silently corrupt the image.  Refuse
 * instead of guessing.
 */
static bool _map_pixfmt(alp_pixfmt_t fmt, lv_color_format_t *lv_fmt, size_t *bytes_per_pixel)
{
	switch (fmt) {
	case ALP_PIXFMT_RGB565:
		*lv_fmt          = LV_COLOR_FORMAT_RGB565;
		*bytes_per_pixel = 2;
		return true;
	case ALP_PIXFMT_RGB888:
		*lv_fmt          = LV_COLOR_FORMAT_RGB888;
		*bytes_per_pixel = 3;
		return true;
	case ALP_PIXFMT_ARGB8888:
		*lv_fmt          = LV_COLOR_FORMAT_ARGB8888;
		*bytes_per_pixel = 4;
		return true;
	default:
		return false;
	}
}

/**
 * @brief LVGL flush callback -- forwards one dirty rect to alp_display_blit().
 *
 * Byte-order note: LVGL v9's LV_COLOR_FORMAT_RGB565 (and RGB888 /
 * ARGB8888) render into `px_map` as plain native-endian C values -- the
 * same in-memory layout the zephyr_drv display backend's
 * PIXEL_FORMAT_RGB_565 buffer expects (src/backends/display/
 * zephyr_drv.c passes the caller's buffer straight through to
 * display_write() with no swap).  Both sides therefore agree without a
 * byte-swap step.  If a future backend's panel disagrees (some SPI TFT
 * controllers want the 16-bit word big-endian on the wire), either
 * switch this display's format to LV_COLOR_FORMAT_RGB565_SWAPPED via
 * lv_conf.h, or byte-swap `px_map` here before the blit -- this bridge
 * does neither today.
 *
 * alp_display_blit() has no channel back to LVGL's flush contract (the
 * callback returns void), so a blit failure is not surfaced here; the
 * caller can inspect alp_last_error() out-of-band if it needs to detect
 * a dropped frame.  flush_ready() is always called so the render loop
 * never wedges on a failed rect.
 */
static void _flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	alp_display_t *display = (alp_display_t *)lv_display_get_user_data(disp);
	uint16_t       x       = (uint16_t)area->x1;
	uint16_t       y       = (uint16_t)area->y1;
	uint16_t       w       = (uint16_t)(area->x2 - area->x1 + 1);
	uint16_t       h       = (uint16_t)(area->y2 - area->y1 + 1);

	(void)alp_display_blit(display, x, y, w, h, px_map);

	lv_display_flush_ready(disp);
}

alp_status_t alp_gui_lvgl_attach(alp_display_t *display)
{
	if (display == NULL) return ALP_ERR_INVAL;

	alp_display_caps_t caps;
	alp_status_t       rc = alp_display_get_caps(display, &caps);
	if (rc != ALP_OK) {
		return rc;
	}

	lv_color_format_t lv_fmt;
	size_t            bytes_per_pixel;
	if (!_map_pixfmt(caps.format, &lv_fmt, &bytes_per_pixel)) {
		return ALP_ERR_NOSUPPORT;
	}

	lv_display_t *disp = lv_display_create(caps.width, caps.height);
	if (disp == NULL) {
		return ALP_ERR_NOMEM;
	}

	lv_display_set_user_data(disp, display);
	lv_display_set_color_format(disp, lv_fmt);

	/* Clamp to the panel height: a display shorter than the configured
	 * line count would otherwise over-allocate for no benefit. */
	uint16_t lines = (uint16_t)CONFIG_ALP_GUI_LVGL_BUF_LINES;
	if (lines > caps.height) {
		lines = caps.height;
	}
	size_t buf_size = (size_t)caps.width * lines * bytes_per_pixel;

	void *buf = malloc(buf_size);
	if (buf == NULL) {
		lv_display_delete(disp);
		return ALP_ERR_NOMEM;
	}

	/* Persistent for the process lifetime -- alp_gui_lvgl_attach() has
	 * no matching detach in the [ABI-STABLE] contract, so there is no
	 * hand-off point to free this at (mirrors the display handle's own
	 * lifetime: one attach per boot). */
	lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, _flush_cb);

	return ALP_OK;
}

#else /* !ALP_HAS_LVGL */

alp_status_t alp_gui_lvgl_attach(alp_display_t *display)
{
	if (display == NULL) return ALP_ERR_INVAL;

	/* No build wires the real LVGL hand-off in -- ALP_HAS_LVGL is unset
     * (see <alp/gui.h>). Degrade to NOSUPPORT rather than claim a
     * binding that doesn't exist. */
	return ALP_ERR_NOSUPPORT;
}

#endif /* ALP_HAS_LVGL */
