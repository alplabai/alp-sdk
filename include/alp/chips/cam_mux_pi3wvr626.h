/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cam_mux_pi3wvr626.h
 * @brief Diodes / Pericom PI3WVR626 2:1 MIPI CSI multiplexer helper.
 *
 * The PI3WVR626 routes one MIPI CSI lane pair (CLK + 2 data lanes)
 * from one of two camera inputs (A or B) to a single output.  The
 * chip exposes only two control pins -- SEL (input switch) and
 * /OE (output enable, typically tied low).  This helper is a
 * GPIO-driven thin shim; it doesn't speak any bus protocol.
 *
 * On the E1M EVK the SEL pin is driven by E1M IO2 (Alif P12.5);
 * /OE is hardwired to GND so the output is always live.  See
 * `<alp/boards/alp_e1m_evk_aen.h>` for the EVK-side macro.
 *
 * Per the datasheet truth table:
 *   SEL = 0  -> A inputs route to outputs (A_MIPI_CSI_*)
 *   SEL = 1  -> B inputs route to outputs (B_MIPI_CSI_*)
 *
 * Switching cameras takes ~10 ns of mux propagation delay; if the
 * downstream MIPI CSI receiver is currently locked, the host
 * should stop the active capture before flipping SEL and re-arm
 * after.
 */

#ifndef ALP_CHIPS_CAM_MUX_PI3WVR626_H
#define ALP_CHIPS_CAM_MUX_PI3WVR626_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAM_MUX_PI3WVR626_INPUT_A = 0,
    CAM_MUX_PI3WVR626_INPUT_B = 1,
} cam_mux_pi3wvr626_input_t;

typedef struct {
    bool        initialised;
    alp_gpio_t *sel_pin;
} cam_mux_pi3wvr626_t;

/**
 * @brief Bind to the SEL GPIO and configure it as an output.
 *
 * @param[in] ctx      Helper context.
 * @param[in] sel_pin  Open GPIO handle for the SEL line (E1M IO2
 *                     on the EVK).  Caller owns the handle.
 */
alp_status_t cam_mux_pi3wvr626_init(cam_mux_pi3wvr626_t *ctx, alp_gpio_t *sel_pin);

/** @brief Switch the mux to input A (SEL=0) or input B (SEL=1). */
alp_status_t cam_mux_pi3wvr626_select(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t input);

/** @brief Read the currently-driven SEL state. */
alp_status_t cam_mux_pi3wvr626_get(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t *input_out);

void         cam_mux_pi3wvr626_deinit(cam_mux_pi3wvr626_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CAM_MUX_PI3WVR626_H */
