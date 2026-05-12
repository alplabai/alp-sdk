/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/camera.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_camera_open() returns NULL.  The real
 * Zephyr-video integration lands in v0.2 (see VERSIONS.md).
 *
 * The stub exists so applications that #include <alp/camera.h>
 * can link cleanly against v0.1 — they discover the lack of
 * support at runtime via a NULL handle / ALP_ERR_NOSUPPORT, the
 * documented v0.1 contract.
 */

#include "alp/camera.h"

alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_camera_start(alp_camera_t *c) {
    (void)c;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_camera_stop(alp_camera_t *c) {
    (void)c;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_camera_capture(alp_camera_t *c,
                                alp_camera_frame_t *out,
                                uint32_t timeout_ms) {
    (void)c; (void)out; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *frame) {
    (void)c; (void)frame;
    return ALP_ERR_NOSUPPORT;
}

void alp_camera_close(alp_camera_t *c) {
    (void)c;
}

alp_status_t alp_camera_configure_isp(alp_camera_t *camera, const alp_camera_isp_config_t *isp)
{
    if (isp == NULL) return ALP_ERR_INVAL;
    (void)camera;
    /* Backends without an on-die ISP return NOSUPPORT.  The AEN
     * Mali-C55 HAL wiring lands once the vendor pack registers a
     * Zephyr-side ISP-config callback on the camera device. */
    return ALP_ERR_NOSUPPORT;
}
