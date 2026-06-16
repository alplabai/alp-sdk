/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL viewer for ai-camera-viewer.  Renders:
 *   - The captured preview frame as a 240×240 LVGL image (top).
 *   - Up to 8 bounding boxes overlaid as borderless rectangles.
 *   - A status strip with per-invoke latency + FPS + camera/
 *     inference health flags.
 */

#include <stdio.h>

#include <lvgl.h>

#include "viewer_ui.h"

static lv_obj_t *s_preview;
static lv_obj_t *s_box_widgets[VIEWER_MAX_BOXES];
static lv_obj_t *s_lbl_stats;
static lv_obj_t *s_lbl_health;

void viewer_ui_build(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	/* Preview pane.  Real capture path fills this with the
     * camera frame; v0.5 fallback paints it a colour as a
     * visible "no frame" placeholder. */
	s_preview = lv_obj_create(scr);
	lv_obj_set_size(s_preview, 240, 240);
	lv_obj_set_pos(s_preview, 0, 0);
	lv_obj_set_style_bg_color(s_preview, lv_color_hex(0x202830), 0);
	lv_obj_set_style_border_width(s_preview, 0, 0);

	/* Pre-allocate the bounding-box widgets so updates don't
     * thrash the LVGL allocator each frame. */
	for (int i = 0; i < VIEWER_MAX_BOXES; i++) {
		s_box_widgets[i] = lv_obj_create(s_preview);
		lv_obj_set_size(s_box_widgets[i], 1, 1);
		lv_obj_set_pos(s_box_widgets[i], 0, 0);
		lv_obj_set_style_bg_opa(s_box_widgets[i], LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_color(s_box_widgets[i], lv_color_hex(0x00FF80), 0);
		lv_obj_set_style_border_width(s_box_widgets[i], 2, 0);
		lv_obj_add_flag(s_box_widgets[i], LV_OBJ_FLAG_HIDDEN);
	}

	/* Stats strip. */
	s_lbl_stats = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_stats, 4, 245);
	lv_obj_set_style_text_color(s_lbl_stats, lv_color_white(), 0);
	lv_label_set_text(s_lbl_stats, "-- fps  --.- ms");

	s_lbl_health = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_health, 4, 285);
	lv_obj_set_style_text_color(s_lbl_health, lv_color_hex(0xA0A0A0), 0);
	lv_label_set_text(s_lbl_health, "cam: ?  inf: ?");
}

void viewer_ui_apply(const viewer_state_t *s)
{
	char buf[64];

	/* Bounding-box overlay.  Hide unused box widgets. */
	for (int i = 0; i < VIEWER_MAX_BOXES; i++) {
		if (i < s->n_boxes) {
			const viewer_box_t *b = &s->boxes[i];
			lv_obj_set_pos(s_box_widgets[i], b->x, b->y);
			lv_obj_set_size(s_box_widgets[i], b->w, b->h);
			lv_obj_clear_flag(s_box_widgets[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(s_box_widgets[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	/* Stats strip. */
	snprintf(buf,
	         sizeof(buf),
	         "%2u.%u fps  %5u µs/inv",
	         (unsigned)(s->fps_x10 / 10),
	         (unsigned)(s->fps_x10 % 10),
	         (unsigned)s->last_invoke_us);
	lv_label_set_text(s_lbl_stats, buf);

	snprintf(buf,
	         sizeof(buf),
	         "cam: %s  inf: %s  boxes: %u",
	         s->camera_ok ? "OK" : "!!",
	         s->inference_ok ? "OK" : "!!",
	         (unsigned)s->n_boxes);
	lv_label_set_text(s_lbl_health, buf);
}
