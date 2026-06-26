/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ics_43434.h
 * @brief InvenSense ICS-43434 omnidirectional MEMS microphone (I²S).
 *
 * Digital MEMS microphone with on-chip ΔΣ ADC.  No configuration
 * register file -- channel-select (L/R) and clock polarity are set
 * via the L/R-SEL pin and the host's I²S controller.  This driver
 * binds the per-channel selection only; sample capture flows through
 * the portable `<alp/i2s.h>` peripheral surface.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: InvenSense ICS-43434 DS-000069 Rev 1.2 (Jul 2017).
 */

#ifndef ALP_CHIPS_ICS_43434_H
#define ALP_CHIPS_ICS_43434_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-channel L/R selection.
 *
 * Set physically by the microphone's L/R-SEL strap and recorded here so the
 * I²S controller decodes the correct slot.  The mic drives data on the WS
 * half that matches this selection.
 */
typedef enum {
	ICS_43434_CH_LEFT  = 0, /**< Mic strapped LEFT (data on WS low phase). */
	ICS_43434_CH_RIGHT = 1, /**< Mic strapped RIGHT (data on WS high phase). */
} ics_43434_channel_t;

/** @brief Driver context.  Caller-allocated; treat fields as opaque. */
typedef struct {
	ics_43434_channel_t channel;     /**< Channel recorded at init (matches the L/R strap). */
	bool                initialised; /**< True once ics_43434_init() has run. */
} ics_43434_t;

/**
 * @brief Bind context with the (caller-strapped) channel assignment.
 *
 * @param dev      Output: caller-allocated context.
 * @param channel  L/R channel selection (must match the L/R-SEL strap).
 * @return `ALP_OK` on success.
 */
alp_status_t ics_43434_init(ics_43434_t *dev, ics_43434_channel_t channel);

/**
 * @brief Return the configured channel (introspection helper).
 *
 * @param dev          Initialised context.
 * @param channel_out  Receives the channel recorded at init.
 * @return `ALP_OK` on success; `ALP_ERR_INVAL` on a NULL argument.
 */
alp_status_t ics_43434_get_channel(ics_43434_t *dev, ics_43434_channel_t *channel_out);

/**
 * @brief Release driver context.
 *
 * Clears the context's initialised flag.  The microphone has no registers to
 * tear down; sample capture is owned by the I²S peripheral, not this driver.
 *
 * @param dev  Context to release (may be NULL).
 */
void ics_43434_deinit(ics_43434_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ICS_43434_H */
