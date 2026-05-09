/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file signal.h
 * @brief ALP SDK signal-processing library.
 *
 * v0.1: re-exports CMSIS-DSP filter primitives via <alp/math.h>
 * and ships ALP-prefixed convenience wrappers for the most common
 * sensor preprocessing flows (FIR, biquad, FFT framing).
 *
 * v0.2 adds an audio submodule: I2S/PDM capture binding,
 * ring-buffered framer, and MFCC frontend.
 */

#ifndef ALP_SIGNAL_H
#define ALP_SIGNAL_H

#include "alp/math.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No new public functions in v0.1 — the surface lives in
 * <alp/math.h>.  This header is a forward marker so app code can
 * #include <alp/signal.h> and remain source-compatible when v0.2
 * adds dedicated wrappers. */

#ifdef __cplusplus
}
#endif

#endif  /* ALP_SIGNAL_H */
