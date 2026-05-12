/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.h
 * @brief ALP SDK camera abstraction (stub for v0.1).
 *
 * v0.2 ships a real MIPI CSI-2 wrapper for the V2N family.  v0.1
 * declares the surface so app code can compile against it; the
 * implementation returns ALP_ERR_NOSUPPORT on every backend.
 */

#ifndef ALP_CAMERA_H
#define ALP_CAMERA_H

#include <stdint.h>
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alp_camera alp_camera_t;

typedef struct {
    uint32_t camera_id;
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    alp_pixfmt_t format;
} alp_camera_config_t;

typedef struct {
    void    *data;
    size_t   size;
    uint64_t timestamp_us;
} alp_camera_frame_t;

/** @brief Open a camera capture handle. */
alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg);

/** @brief Start streaming.  Frames become available via @ref alp_camera_capture. */
alp_status_t alp_camera_start(alp_camera_t *c);

/** @brief Stop streaming.  Backend may keep the handle warm for restart. */
alp_status_t alp_camera_stop(alp_camera_t *c);

/** @brief Block until next frame.  Caller does not own the frame buffer. */
alp_status_t alp_camera_capture(alp_camera_t *c,
                                alp_camera_frame_t *out,
                                uint32_t timeout_ms);

/** @brief Return the frame buffer to the backend after the caller has consumed it. */
alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *frame);

/** @brief Close the handle and release backend resources.  Idempotent. */
void alp_camera_close(alp_camera_t *c);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_CAMERA_H */
