/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AI_CAMERA_VIEWER_UI_H
#define AI_CAMERA_VIEWER_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIEWER_MAX_BOXES 8

typedef struct {
    int16_t x, y, w, h;
    uint8_t class_id;
    float   score;
} viewer_box_t;

typedef struct {
    viewer_box_t boxes[VIEWER_MAX_BOXES];
    uint8_t      n_boxes;
    uint32_t     last_invoke_us;
    uint32_t     fps_x10;          /**< Frames-per-second × 10 (one decimal). */
    bool         camera_ok;
    bool         inference_ok;
} viewer_state_t;

void viewer_ui_build(void);
void viewer_ui_apply(const viewer_state_t *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AI_CAMERA_VIEWER_UI_H */
