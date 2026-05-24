/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AI_CAMERA_VIEWER_INFERENCE_LOOP_H
#define AI_CAMERA_VIEWER_INFERENCE_LOOP_H

#include "viewer_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Inference thread entrypoint.  Owns the camera + inference
 *  handles, captures one frame per cycle, runs the model,
 *  decodes the output tensor, and updates the shared state. */
void inference_loop_run(viewer_state_t *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AI_CAMERA_VIEWER_INFERENCE_LOOP_H */
