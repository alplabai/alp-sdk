/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL HUD layout for drone-hud.  Builds the on-screen widgets
 * once + updates them per-frame from a telemetry snapshot.
 */

#ifndef DRONE_HUD_HUD_UI_H
#define DRONE_HUD_HUD_UI_H

#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the HUD layout (called once after lv_init()). */
void hud_ui_build(void);

/** Push the latest telemetry into the on-screen widgets.  Called
 *  once per render frame from main()'s LVGL loop. */
void hud_ui_apply_telemetry(const drone_telemetry_t *t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_HUD_HUD_UI_H */
