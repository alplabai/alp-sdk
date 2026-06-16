/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL HUD layout for drone-hud.
 *
 * Renders:
 *   - Top half: artificial horizon (a tilted rectangle representing
 *     the sky/ground line).  Rotates around the screen centre as a
 *     function of roll; shifts vertically as a function of pitch.
 *   - Bottom half: 3 telemetry blocks
 *       * GPS:     lat / lon / sat-count / fix.
 *       * Battery: V / A / minutes remaining.
 *       * Mode:    current flight mode selector.
 *
 * Keeping the layout in C (not a JSON asset) means the demo
 * compiles standalone without an asset-loading step -- builds
 * on native_sim with zero external state.
 */

#include <stdio.h>

#include <lvgl.h>

#include "hud_ui.h"

/* Widget handles updated by apply_telemetry().  File-scope so the
 * update fn doesn't need a context struct. */
static lv_obj_t *s_horizon;
static lv_obj_t *s_lbl_attitude;
static lv_obj_t *s_lbl_gps;
static lv_obj_t *s_lbl_batt;
static lv_obj_t *s_lbl_mode;

void hud_ui_build(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	/* ── Artificial horizon ────────────────────────────────────
     *
     * A simple sky / ground line: top half blue, bottom half
     * brown, with a centre crosshair.  Rotates on roll, shifts
     * on pitch.  Real EFIS displays do far more (heading scale,
     * pitch ladder, bank angle indicator) -- this is enough to
     * showcase the data flow.
     */
	s_horizon = lv_obj_create(scr);
	lv_obj_set_size(s_horizon, 240, 120);
	lv_obj_set_pos(s_horizon, 0, 0);
	lv_obj_set_style_bg_color(s_horizon, lv_color_hex(0x3060A0), 0);     /* sky */
	lv_obj_set_style_border_color(s_horizon, lv_color_hex(0x8B4513), 0); /* ground */
	lv_obj_set_style_border_width(s_horizon, 60, 0);
	lv_obj_set_style_border_side(s_horizon, LV_BORDER_SIDE_BOTTOM, 0);

	/* Attitude readout overlay. */
	s_lbl_attitude = lv_label_create(scr);
	lv_obj_set_style_text_color(s_lbl_attitude, lv_color_white(), 0);
	lv_obj_align(s_lbl_attitude, LV_ALIGN_TOP_MID, 0, 5);
	lv_label_set_text(s_lbl_attitude, "R:--.- P:--.- Y:---.-");

	/* ── GPS block ─────────────────────────────────────────────*/
	s_lbl_gps = lv_label_create(scr);
	lv_obj_set_style_text_color(s_lbl_gps, lv_color_hex(0x40FF40), 0);
	lv_obj_set_pos(s_lbl_gps, 4, 130);
	lv_label_set_text(s_lbl_gps, "GPS: no fix");

	/* ── Battery block ─────────────────────────────────────────*/
	s_lbl_batt = lv_label_create(scr);
	lv_obj_set_style_text_color(s_lbl_batt, lv_color_hex(0xFFD060), 0);
	lv_obj_set_pos(s_lbl_batt, 4, 200);
	lv_label_set_text(s_lbl_batt, "BAT: --.--V / --.--A");

	/* ── Mode selector readout ─────────────────────────────────*/
	s_lbl_mode = lv_label_create(scr);
	lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_pos(s_lbl_mode, 4, 270);
	lv_label_set_text(s_lbl_mode, "MODE: --");
}

void hud_ui_apply_telemetry(const drone_telemetry_t *t)
{
	char buf[64];

	/* Attitude.  Tilt the horizon widget on roll; shift on pitch. */
	lv_obj_set_style_transform_rotation(s_horizon, (int16_t)(t->roll_deg * 10), 0);
	lv_obj_set_pos(s_horizon, 0, (int)(t->pitch_deg * 0.5f));

	snprintf(buf,
	         sizeof(buf),
	         "R:%+6.1f P:%+6.1f Y:%6.1f",
	         (double)t->roll_deg,
	         (double)t->pitch_deg,
	         (double)t->yaw_deg);
	lv_label_set_text(s_lbl_attitude, buf);

	/* GPS. */
	if (t->gps_fix) {
		snprintf(buf,
		         sizeof(buf),
		         "GPS %d sats\n%.5f, %.5f",
		         (int)t->sat_count,
		         (double)t->lat_deg,
		         (double)t->lon_deg);
	} else {
		snprintf(buf, sizeof(buf), "GPS: %d sats (no fix)", (int)t->sat_count);
	}
	lv_label_set_text(s_lbl_gps, buf);

	/* Battery. */
	snprintf(buf,
	         sizeof(buf),
	         "BAT %5.2fV %+5.2fA\nremain %3ld min",
	         (double)t->battery_v,
	         (double)t->battery_a,
	         (long)t->battery_remaining_min);
	lv_label_set_text(s_lbl_batt, buf);

	/* Mode. */
	static const char *names[] = {
		[DRONE_MODE_MANUAL]     = "MANUAL",
		[DRONE_MODE_STABILISED] = "STAB",
		[DRONE_MODE_GPS_HOLD]   = "GPS-HOLD",
	};
	snprintf(buf, sizeof(buf), "MODE: %s", names[t->mode <= DRONE_MODE_GPS_HOLD ? t->mode : 0]);
	lv_label_set_text(s_lbl_mode, buf);
}
