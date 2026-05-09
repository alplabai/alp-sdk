/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/audio.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_audio_in_open / alp_audio_out_open return
 * NULL.  Real PDM/I²S backend lands in v0.2 (see VERSIONS.md).
 *
 * Same shape as iot_stub.c / camera_stub.c.
 */

#include "alp/audio.h"

/* ------------------------------------------------------------------ */
/* Audio input                                                         */
/* ------------------------------------------------------------------ */

alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_audio_in_start(alp_audio_in_t *in) {
    (void)in;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_audio_in_stop(alp_audio_in_t *in) {
    (void)in;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void *buf, size_t frames,
                               size_t *out_frames,
                               uint32_t timeout_ms) {
    (void)in; (void)buf; (void)frames; (void)timeout_ms;
    if (out_frames != NULL) *out_frames = 0;
    return ALP_ERR_NOSUPPORT;
}

void alp_audio_in_close(alp_audio_in_t *in) {
    (void)in;
}

/* ------------------------------------------------------------------ */
/* Audio output                                                        */
/* ------------------------------------------------------------------ */

alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_audio_out_start(alp_audio_out_t *out) {
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_audio_out_stop(alp_audio_out_t *out) {
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void *buf, size_t frames,
                                 size_t *out_frames,
                                 uint32_t timeout_ms) {
    (void)out; (void)buf; (void)frames; (void)timeout_ms;
    if (out_frames != NULL) *out_frames = 0;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol) {
    (void)out; (void)vol;
    return ALP_ERR_NOSUPPORT;
}

void alp_audio_out_close(alp_audio_out_t *out) {
    (void)out;
}
