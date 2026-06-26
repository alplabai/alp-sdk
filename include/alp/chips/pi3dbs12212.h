/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pi3dbs12212.h
 * @brief Diodes Inc PI3DBS12212A 2:1 high-speed differential mux.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The PI3DBS12212A is a passive 4-into-2 differential mux/demux (or
 * equivalently, a 2-position 1-lane switch).  3.3 V supply, up to
 * 12 Gbps per channel, supports PCIe v3.0, USB 3.0/3.1, Thunderbolt,
 * SATA3.0, MIPI D-PHY -- it doesn't care about the protocol, just
 * the differential pair shape.
 *
 * @par Control surface
 *
 * The chip is **GPIO-only** -- no I2C / SPI control surface.  Two
 * digital inputs:
 *
 *   - `PD` -- power-down / disable.  Driven low to disable the mux
 *     (high-Z all outputs); driven high to enable.  3.3 V LVTTL.
 *   - `SEL` -- path select between the two input pairs.  3.3 V LVTTL.
 *
 * On V2N-M1 these are wired to the Renesas RZ/V2N:
 *
 *   - `PCIe.MUX_PD` on `P80` -- the chip is held in power-down at
 *     reset; firmware deasserts after the PCIe rails come up.
 *   - `PCIe.MUX_SEL` on `P95` -- selects between **DEEPX** (on-module
 *     DX-M1 NPU) and **E1M-edge** PCIe routing.
 *
 * Two physical PI3DBS12212A instances are placed on the V2N-M1 board
 * (one for the PCIe lane's TX+/- and another for RX+/-); both share
 * the same `PD` + `SEL` lines so a single driver context drives the
 * pair.
 *
 * @par Driver model
 *
 * Three logical states the firmware can request:
 *
 *   - `PI3DBS_STATE_OFF` -- mux disabled (`PD = 0`).  All outputs
 *     high-Z; safe default at boot.
 *   - `PI3DBS_STATE_PATH_0` -- enabled, routing input pair 0 -> output.
 *   - `PI3DBS_STATE_PATH_1` -- enabled, routing input pair 1 -> output.
 *
 * The "which path is DEEPX vs E1M edge" mapping is a board-board
 * convention; the driver is agnostic.  Boards document the mapping
 * in `metadata/e1m_modules/v2n-m1/README.md` and in
 * `metadata/chips/pi3dbs12212.yaml`.
 *
 * @par Datasheet provenance
 * - **PI3DBS12212A datasheet DS40393 Rev 3-2 (Sep 2020)** from Diodes
 *   Inc -- archived vendor documentation
 *   tree.
 */

#ifndef ALP_CHIPS_PI3DBS12212_H
#define ALP_CHIPS_PI3DBS12212_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Logical mux state the firmware can request via pi3dbs12212_set_state(). */
typedef enum {
	PI3DBS_STATE_OFF    = 0, /**< Mux disabled (PD = 0); all outputs high-Z. Boot default. */
	PI3DBS_STATE_PATH_0 = 1, /**< Mux enabled, SEL = 0; routes input pair 0 -> output. */
	PI3DBS_STATE_PATH_1 = 2, /**< Mux enabled, SEL = 1; routes input pair 1 -> output. */
} pi3dbs12212_state_t;

/** @brief Driver context for one PI3DBS12212A (or a PD/SEL-ganged pair). */
typedef struct {
	bool                initialised; /**< True once pi3dbs12212_init() has bound the pins. */
	alp_gpio_t         *pd;          /**< PD pin (high = mux enabled). Owned after init. */
	alp_gpio_t         *sel;         /**< SEL pin (path select). Owned after init. */
	pi3dbs12212_state_t state;       /**< Last state driven (chip has no readback). */
} pi3dbs12212_t;

/**
 * @brief Initialise the driver and put the mux in OFF.
 *
 * Takes ownership of the two pre-opened GPIO handles.  Both must
 * already have been configured as outputs by the caller; the
 * driver only drives them.
 *
 * @param ctx Caller-allocated context to populate.
 * @param pd  Pre-opened output GPIO for the PD pin (high = enabled).
 * @param sel Pre-opened output GPIO for the SEL pin (path select).
 * @return ALP_OK on success; ALP_ERR_INVAL on any NULL argument.
 */
alp_status_t pi3dbs12212_init(pi3dbs12212_t *ctx, alp_gpio_t *pd, alp_gpio_t *sel);

/**
 * @brief Transition to a new logical state.
 *
 * Drives PD + SEL in the correct order (off-then-on for path changes
 * to avoid glitching the link).
 *
 * @param ctx   Initialised driver context.
 * @param state Target state (see ::pi3dbs12212_state_t).
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL ctx or invalid state;
 *         an error status from the underlying GPIO writes otherwise.
 */
alp_status_t pi3dbs12212_set_state(pi3dbs12212_t *ctx, pi3dbs12212_state_t state);

/**
 * @brief Read back the current state (driver-side cache).
 *
 * The chip itself has no readback; this returns the last state driven.
 *
 * @param ctx   Initialised driver context.
 * @param state Out-param; receives the cached state.
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL argument.
 */
alp_status_t pi3dbs12212_get_state(pi3dbs12212_t *ctx, pi3dbs12212_state_t *state);

/**
 * @brief Release the driver context.  Drives the mux to OFF first.
 *
 * @param ctx Driver context; NULL is tolerated as a no-op.
 */
void pi3dbs12212_deinit(pi3dbs12212_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_PI3DBS12212_H */
