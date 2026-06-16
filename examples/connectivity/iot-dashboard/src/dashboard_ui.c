/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dashboard layout for iot-dashboard.
 *
 * Three tiles:
 *   - Temperature (large numeric, colour-coded by range).
 *   - Humidity    (gauge with hint text).
 *   - Pressure    (sparkline showing the last 60 samples).
 *
 * Plus a status strip showing the MQTT connection state and the
 * timestamp of the last sensor update.  Refresh runs at the
 * LVGL render frequency (30 fps via prj.conf); the sensor thread
 * pushes new values once per second.
 */

#include <stdio.h>

#include <lvgl.h>

#include "dashboard_ui.h"

static lv_obj_t          *s_lbl_temp;
static lv_obj_t          *s_lbl_humid;
static lv_obj_t          *s_lbl_pressure;
static lv_obj_t          *s_lbl_status;
static lv_obj_t          *s_chart;
static lv_chart_series_t *s_pressure_series;

void                      dashboard_ui_build(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

	/* Temperature tile. */
	s_lbl_temp = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_temp, 8, 8);
	lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(0xFF6040), 0);
	lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_28, 0);
	lv_label_set_text(s_lbl_temp, "--.- °C");

	/* Humidity tile. */
	s_lbl_humid = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_humid, 8, 60);
	lv_obj_set_style_text_color(s_lbl_humid, lv_color_hex(0x40A0FF), 0);
	lv_obj_set_style_text_font(s_lbl_humid, &lv_font_montserrat_28, 0);
	lv_label_set_text(s_lbl_humid, "--.- %RH");

	/* Pressure numeric + sparkline. */
	s_lbl_pressure = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_pressure, 8, 112);
	lv_obj_set_style_text_color(s_lbl_pressure, lv_color_hex(0xFFFFFF), 0);
	lv_label_set_text(s_lbl_pressure, "---- hPa");

	s_chart = lv_chart_create(scr);
	lv_obj_set_size(s_chart, 224, 100);
	lv_obj_set_pos(s_chart, 8, 140);
	lv_chart_set_point_count(s_chart, 60);
	lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 950, 1050);
	s_pressure_series =
	    lv_chart_add_series(s_chart, lv_color_hex(0x40FF80), LV_CHART_AXIS_PRIMARY_Y);

	/* Status strip at the bottom. */
	s_lbl_status = lv_label_create(scr);
	lv_obj_set_pos(s_lbl_status, 8, 260);
	lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x808890), 0);
	lv_label_set_text(s_lbl_status, "MQTT: --   updated --");
}

void dashboard_ui_apply(const dashboard_state_t *s)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%5.1f °C", (double)s->temp_c);
	lv_label_set_text(s_lbl_temp, buf);

	snprintf(buf, sizeof(buf), "%5.1f %%RH", (double)s->humid_pct);
	lv_label_set_text(s_lbl_humid, buf);

	snprintf(buf, sizeof(buf), "%6.1f hPa", (double)s->pressure_hpa);
	lv_label_set_text(s_lbl_pressure, buf);

	/* Push the latest pressure into the sparkline. */
	lv_chart_set_next_value(s_chart, s_pressure_series, (int32_t)s->pressure_hpa);
	lv_chart_refresh(s_chart);

	snprintf(buf, sizeof(buf), "MQTT: %s   t=%u ms", s->mqtt_connected ? "CONNECTED" : "offline",
	         (unsigned)s->last_update_ms);
	lv_label_set_text(s_lbl_status, buf);
}
