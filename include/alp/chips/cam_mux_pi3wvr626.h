/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cam_mux_pi3wvr626.h
 * @brief Diodes / Pericom PI3WVR626 2:1 MIPI CSI multiplexer helper.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The PI3WVR626 routes one MIPI CSI lane pair (CLK + 2 data lanes)
 * from one of two camera inputs (A or B) to a single output.  The
 * chip exposes only two control pins -- SEL (input switch) and
 * /OE (output enable, typically tied low).  This helper is a
 * GPIO-driven thin shim; it doesn't speak any bus protocol.
 *
 * On the E1M EVK the SEL pin is driven by E1M IO2 (Alif P12.5);
 * /OE is hardwired to GND so the output is always live.  See
 * `<alp/boards/alp_e1m_evk.h>` for the EVK-side macro.
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

/** @brief Camera input routed to the mux output (datasheet SEL truth table). */
typedef enum {
	CAM_MUX_PI3WVR626_INPUT_A = 0, /**< SEL=0: route the A_MIPI_CSI_* inputs to the output. */
	CAM_MUX_PI3WVR626_INPUT_B = 1, /**< SEL=1: route the B_MIPI_CSI_* inputs to the output. */
} cam_mux_pi3wvr626_input_t;

/**
 * @brief Helper context for one PI3WVR626 mux instance.
 *
 * Caller-allocated and owned for the lifetime of the mux; the fields are
 * driver-private (treat as opaque) and populated by @ref cam_mux_pi3wvr626_init.
 */
typedef struct {
	bool        initialised; /**< True between a successful init and deinit. */
	alp_gpio_t *sel_pin;     /**< Borrowed SEL GPIO handle; the caller retains ownership. */
} cam_mux_pi3wvr626_t;

/**
 * @brief Bind to the SEL GPIO and configure it as an output.
 *
 * @param[in] ctx      Helper context (output).
 * @param[in] sel_pin  Open GPIO handle for the SEL line (E1M IO2
 *                     on the EVK).  Caller owns the handle.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p ctx or @p sel_pin is NULL.
 */
alp_status_t cam_mux_pi3wvr626_init(cam_mux_pi3wvr626_t *ctx, alp_gpio_t *sel_pin);

/**
 * @brief Switch the mux to input A (SEL=0) or input B (SEL=1).
 *
 * Drives SEL synchronously; the ~10 ns mux propagation delay means the host
 * should stop any active downstream MIPI CSI capture before flipping inputs.
 *
 * @param[in] ctx    Initialised helper context.
 * @param[in] input  Which camera input to route to the output.
 * @return ALP_OK once SEL is driven; ALP_ERR_INVAL on a NULL @p ctx or an
 *         out-of-range @p input; otherwise the underlying GPIO-write status.
 */
alp_status_t cam_mux_pi3wvr626_select(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t input);

/**
 * @brief Report which input the SEL line is currently driving.
 *
 * @param[in]  ctx        Initialised helper context.
 * @param[out] input_out  Receives the currently-selected input.
 * @return ALP_OK with @p input_out set; ALP_ERR_INVAL on a NULL argument.
 */
alp_status_t cam_mux_pi3wvr626_get(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t *input_out);

/**
 * @brief Release the driver context.  Idempotent.
 *
 * Does not close the SEL GPIO handle -- the caller owns it.
 *
 * @param[in] ctx  Helper context (may already be deinitialised, or NULL).
 */
void cam_mux_pi3wvr626_deinit(cam_mux_pi3wvr626_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CAM_MUX_PI3WVR626_H */
