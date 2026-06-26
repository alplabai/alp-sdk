/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dashboard layout for iot-dashboard -- the LVGL view layer.
 *
 * This file owns the on-screen widgets and nothing else: it knows how to
 * BUILD the dashboard once and how to RE-PAINT it from a plain data struct.
 * It deliberately has no knowledge of I2C, the BME280, MQTT, or threads --
 * main.c samples the sensor and hands this layer a finished snapshot. Keeping
 * the view free of device code is what lets the same UI run unchanged on the
 * ST7789 panel and on native_sim's framebuffer emulator.
 *
 * The split is the LVGL idiom and the reason for the two entry points:
 *   - dashboard_ui_build() runs ONCE at startup. It creates the widget tree
 *     and stashes each widget handle in a file-static pointer.
 *   - dashboard_ui_apply() runs EVERY frame. It only mutates the text/values
 *     of those cached handles -- no allocation, no re-layout -- so the render
 *     loop stays cheap.
 *
 * Three tiles + a status strip, each a fixed accent colour (no per-value
 * recolouring -- the colour is set once in build()):
 *   - Temperature  large 28px numeric, warm orange.
 *   - Humidity     large 28px numeric, blue.
 *   - Pressure     white numeric plus a green sparkline of recent samples.
 *   - Status strip grey, shows MQTT connection state + last-update timestamp.
 *
 * main.c's sensor thread refreshes the shared state at 1 Hz; the render loop
 * calls dashboard_ui_apply() far more often, so most frames just re-draw the
 * last sample. That is fine -- the UI is a viewer, the sensor sets the pace.
 */

#include <stdio.h>

#include <lvgl.h>

#include "dashboard_ui.h"

/* Widget handles cached at build() time so apply() can update them in place
 * every frame. They stay file-static because LVGL owns the objects (they live
 * on the active screen); we only borrow the pointers to push new values. */
static lv_obj_t          *s_lbl_temp;
static lv_obj_t          *s_lbl_humid;
static lv_obj_t          *s_lbl_pressure;
static lv_obj_t          *s_lbl_status;
static lv_obj_t          *s_chart;
static lv_chart_series_t *s_pressure_series;

void dashboard_ui_build(void)
{
	/* Everything hangs off the active screen so the layout follows whatever
	 * display LVGL was initialised against. Dark near-black background so the
	 * accent-coloured readouts pop on the small panel. */
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

	/* Temperature tile: top of the stack, large font, warm orange accent.
	 * Seeded with an em-dash placeholder so the first frame is sensible
	 * before any BME280 sample has arrived. */
	s_lbl_temp = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_temp, 8, 8);
	lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(0xFF6040), 0);
	lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_28, 0);
	lv_label_set_text(s_lbl_temp, "--.- °C");

	/* Humidity tile: same large font, blue accent to read distinctly from the
	 * temperature line directly above it. */
	s_lbl_humid = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_humid, 8, 60);
	lv_obj_set_style_text_color(s_lbl_humid, lv_color_hex(0x40A0FF), 0);
	lv_obj_set_style_text_font(s_lbl_humid, &lv_font_montserrat_28, 0);
	lv_label_set_text(s_lbl_humid, "--.- %RH");

	/* Pressure: a white numeric readout with a sparkline beneath it, since the
	 * trend over time is more telling than any single barometric reading. */
	s_lbl_pressure = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_pressure, 8, 112);
	lv_obj_set_style_text_color(s_lbl_pressure, lv_color_hex(0xFFFFFF), 0);
	lv_label_set_text(s_lbl_pressure, "---- hPa");

	/* 60-point chart = a rolling one-minute window at the sensor's 1 Hz
	 * cadence. The Y range is pinned to 950..1050 hPa (normal sea-level
	 * weather band) rather than auto-scaled, so small real fluctuations stay
	 * visible and noise doesn't make the line jump to fill the box. */
	s_chart = lv_chart_create(scr);
	lv_obj_set_size(s_chart, 224, 100);
	lv_obj_set_pos(s_chart, 8, 140);
	lv_chart_set_point_count(s_chart, 60);
	lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 950, 1050);
	s_pressure_series =
	    lv_chart_add_series(s_chart, lv_color_hex(0x40FF80), LV_CHART_AXIS_PRIMARY_Y);

	/* Status strip pinned to the bottom: muted grey because it is chrome, not
	 * data -- it reports the MQTT link state and when the last sample landed. */
	s_lbl_status = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_status, 8, 260);
	lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x808890), 0);
	lv_label_set_text(s_lbl_status, "MQTT: --   updated --");
}

/* Re-paint the dashboard from an immutable snapshot. `s` is a by-value copy
 * main.c took of the shared state, so this function never races the sensor
 * thread and never blocks it. Widths in the format strings are fixed so the
 * numbers don't shift left/right as digits appear and disappear. */
void dashboard_ui_apply(const dashboard_state_t *s)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%5.1f °C", (double)s->temp_c);
	lv_label_set_text(s_lbl_temp, buf);

	snprintf(buf, sizeof(buf), "%5.1f %%RH", (double)s->humid_pct);
	lv_label_set_text(s_lbl_humid, buf);

	snprintf(buf, sizeof(buf), "%6.1f hPa", (double)s->pressure_hpa);
	lv_label_set_text(s_lbl_pressure, buf);

	/* Scroll one sample into the sparkline: set_next_value advances LVGL's
	 * internal ring buffer (oldest point drops off the left), then refresh
	 * redraws it. Truncating to int32 is fine -- 1 hPa resolution on a chart
	 * that spans a 100 hPa window is below one pixel. */
	lv_chart_set_next_value(s_chart, s_pressure_series, (int32_t)s->pressure_hpa);
	lv_chart_refresh(s_chart);

	snprintf(buf,
	         sizeof(buf),
	         "MQTT: %s   t=%u ms",
	         s->mqtt_connected ? "CONNECTED" : "offline",
	         (unsigned)s->last_update_ms);
	lv_label_set_text(s_lbl_status, buf);
}
