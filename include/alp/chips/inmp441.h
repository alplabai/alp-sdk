/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file inmp441.h
 * @brief InvenSense INMP441 low-cost omnidirectional MEMS mic (I²S).
 *
 * Lower-cost peer to ICS-43434.  Same I²S sample path; per-channel
 * (L/R) selection via the L/R-SEL pin.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: InvenSense INMP441 DS-INMP441 Rev 1.2 (Mar 2015).
 */

#ifndef ALP_CHIPS_INMP441_H
#define ALP_CHIPS_INMP441_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INMP441_CH_LEFT  = 0,
    INMP441_CH_RIGHT = 1,
} inmp441_channel_t;

typedef struct {
    inmp441_channel_t channel;
    bool              initialised;
} inmp441_t;

/** @brief Bind context with the (strapped) channel assignment. */
alp_status_t inmp441_init(inmp441_t *dev, inmp441_channel_t channel);

/** @brief Read back the configured channel. */
alp_status_t inmp441_get_channel(inmp441_t *dev, inmp441_channel_t *channel_out);

/** @brief Release driver context. */
void inmp441_deinit(inmp441_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_INMP441_H */
