/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IOT_DASHBOARD_UI_H
#define IOT_DASHBOARD_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    temp_c;
    float    humid_pct;
    float    pressure_hpa;
    bool     mqtt_connected;
    uint32_t last_update_ms;
} dashboard_state_t;

void dashboard_ui_build(void);
void dashboard_ui_apply(const dashboard_state_t *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IOT_DASHBOARD_UI_H */
