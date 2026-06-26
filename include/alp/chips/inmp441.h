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

/**
 * @brief I²S time-slot the mic drives, selected in hardware by the L/R-SEL pin.
 *
 * This is informational only: the mic samples on whichever slot its L/R-SEL
 * strap selects.  The value records that strap so the I²S consumer reads the
 * matching slot; it does not reconfigure the part.
 */
typedef enum {
	INMP441_CH_LEFT  = 0, /**< Mic drives the left slot (L/R-SEL tied low). */
	INMP441_CH_RIGHT = 1, /**< Mic drives the right slot (L/R-SEL tied high). */
} inmp441_channel_t;

/** @brief Driver context for one INMP441 mic.  Treat as opaque. */
typedef struct {
	inmp441_channel_t channel;     /**< Strapped channel assignment recorded at init. */
	bool              initialised; /**< True once inmp441_init() succeeded. */
} inmp441_t;

/**
 * @brief Bind a context, recording the mic's strapped channel assignment.
 *
 * Config-only: there is no register bus, so this just validates and stores the
 * channel.  Audio samples arrive over the I²S peripheral, not through this API.
 *
 * @param[out] dev      Context to populate.
 * @param[in]  channel  Channel the L/R-SEL pin straps the mic to.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p dev is NULL or @p channel is
 *         not a valid @ref inmp441_channel_t.
 */
alp_status_t inmp441_init(inmp441_t *dev, inmp441_channel_t channel);

/**
 * @brief Read back the configured channel assignment.
 *
 * @param[in]  dev          Initialised context.
 * @param[out] channel_out  Receives the strapped channel.
 * @return ALP_OK on success; ALP_ERR_NOT_READY if @p dev is NULL or
 *         uninitialised; ALP_ERR_INVAL if @p channel_out is NULL.
 */
alp_status_t inmp441_get_channel(inmp441_t *dev, inmp441_channel_t *channel_out);

/**
 * @brief Release the driver context.  NULL tolerated.
 *
 * @param[in,out] dev  Context to release (may be NULL).
 */
void inmp441_deinit(inmp441_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_INMP441_H */
