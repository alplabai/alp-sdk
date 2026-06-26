/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file deepx_dxm1.h
 * @brief DEEPX DX-M1 NPU host-side bring-up sequencer (V2N-M1 only).
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The DX-M1 is the AI accelerator on the **E1M-X V2N-M1** family
 * of SoMs (NOT populated on V2N base).  It sits behind a PCIe lane
 * that V2N-M1 routes through two passive Diodes PI3DBS12212A
 * differential muxes — host firmware must:
 *
 *   1. Hold `M1_RESET` (Renesas `PA6`) asserted.
 *   2. Bring the DEEPX rails up (DA9292 ch3+ch4 to 0.75 V + the
 *      three DEEPX-specific TPS628640 buck instances on BRD_I2C).
 *      Driver-level helpers for those live in
 *      [`<alp/chips/da9292.h>`](da9292.h) and
 *      [`<alp/chips/tps628640.h>`](tps628640.h); this header does
 *      not orchestrate the rail bring-up itself -- board-board
 *      firmware composes the helpers + this driver in the right
 *      order.
 *   3. Configure the PCIe muxes to the DEEPX path then enable them
 *      (via [`<alp/chips/pi3dbs12212.h>`](pi3dbs12212.h)).
 *   4. Release `M1_RESET`.
 *   5. Wait `boot_us` for DEEPX firmware to come online.
 *   6. Hand control to the Linux kernel driver
 *      (`dx_rt_npu_linux_driver`).
 *
 * Public API exposes the sequencer as a single
 * [`deepx_dxm1_bring_up()`](deepx_dxm1_bring_up) call that takes
 * pre-opened GPIO + mux contexts and runs steps 3..5.  Steps 1-2
 * are the caller's responsibility (the secondary PMIC + buck
 * drivers ship the right helpers, but composing them depends on
 * board-board design decisions).
 *
 * @par Datasheet provenance
 * DEEPX DX-M1 commercial + M.2-card datasheets are held in the
 * project's vendor datasheet (not
 * redistributed with this repo).
 */

#ifndef ALP_CHIPS_DEEPX_DXM1_H
#define ALP_CHIPS_DEEPX_DXM1_H

#include <stdbool.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/chips/pi3dbs12212.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Conservative default boot delay (microseconds) between releasing
 *  `M1_RESET` and handing off to the Linux driver.  Refine once the
 *  DEEPX datasheet's "POR-to-PCIe-link-up" figure is read out --
 *  start with 50 ms which is safe for every reasonable boot ROM. */
#define DEEPX_DXM1_DEFAULT_BOOT_US 50000u

/** Polarity of the DEEPX `M1_RESET` line.  Confirmed
 *  **active-LOW** for V2N-M1 (2026-05-12 maintainer decision):
 *  the host drives `M1_RESET` low to hold reset, high to release.
 *  Board boards on different DEEPX revisions can flip via
 *  @ref deepx_dxm1_set_reset_polarity() if the silicon polarity
 *  ever changes. */
typedef enum {
	DEEPX_DXM1_RESET_ACTIVE_LOW  = 0, /**< Default on V2N-M1: drive low to hold reset. */
	DEEPX_DXM1_RESET_ACTIVE_HIGH = 1, /**< Drive high to hold reset (other DEEPX revs). */
} deepx_dxm1_reset_polarity_t;

/** @brief DEEPX DX-M1 bring-up sequencer context.  Caller-allocated; opaque to apps. */
typedef struct {
	bool                        initialised;    /**< True between a successful init and deinit. */
	alp_gpio_t                 *m1_reset_pin;   /**< Renesas PA6.        */
	pi3dbs12212_t              *pcie_mux;       /**< owned + opened by caller. */
	pi3dbs12212_state_t         deepx_path;     /**< Which mux state means "to DEEPX". */
	deepx_dxm1_reset_polarity_t reset_polarity; /**< Active level of M1_RESET (default low). */
} deepx_dxm1_t;

/**
 * @brief Bind the sequencer to the caller-supplied GPIO + mux contexts.
 *
 * Drives M1_RESET to its asserted state and leaves the mux in OFF
 * (PI3DBS_STATE_OFF) so subsequent steps execute from a known
 * quiescent state.
 *
 * @param ctx          Driver context (output).
 * @param m1_reset     alp_gpio_t handle for Renesas `PA6`, configured
 *                     as an output by the caller.
 * @param pcie_mux     pi3dbs12212_t context already initialised
 *                     against the two PD + SEL GPIOs (`P80`, `P95`).
 * @param deepx_path   Which mux state the board has wired to the
 *                     DEEPX side (board convention --
 *                     `PI3DBS_STATE_PATH_0` is the typical V2N-M1
 *                     mapping per `metadata/chips/pi3dbs12212.yaml`).
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t deepx_dxm1_init(deepx_dxm1_t       *ctx,
                             alp_gpio_t         *m1_reset,
                             pi3dbs12212_t      *pcie_mux,
                             pi3dbs12212_state_t deepx_path);

/**
 * @brief Override the assumed reset-line polarity.
 *
 * The default after @ref deepx_dxm1_init is active-low (the confirmed V2N-M1
 * polarity); call this only for a board whose DEEPX silicon flips it.  Takes
 * effect on the next @ref deepx_dxm1_bring_up / @ref deepx_dxm1_shut_down.
 *
 * @param ctx  Initialised sequencer context.
 * @param p    Reset-line polarity to assume from now on.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL @p ctx.
 */
alp_status_t deepx_dxm1_set_reset_polarity(deepx_dxm1_t *ctx, deepx_dxm1_reset_polarity_t p);

/**
 * @brief Run the bring-up sequence: route the PCIe muxes to DEEPX,
 *        enable them, then release `M1_RESET`.
 *
 * Pre-conditions (caller's responsibility):
 *   - DEEPX rails (0.75 V DEEPX-rail via DA9292 ch3+ch4; the three
 *     DEEPX-specific TPS628640 instances on BRD_I2C) are stable and
 *     have been at their target voltages for ≥ 1 ms.
 *   - The PCIe controller on the Renesas side is initialised but
 *     not yet attempting link-up.
 *
 * Post-conditions:
 *   - M1_RESET is in the "run" state.
 *   - PCIe muxes are routing to DEEPX.
 *   - Linux kernel driver can attempt `dxrt_init()` after the boot
 *     delay has elapsed.
 *
 * @param ctx      DEEPX DX-M1 sequencer context (must be initialised first).
 * @param boot_us  Delay (microseconds) the sequencer waits between
 *                 releasing M1_RESET and returning.  Pass 0 to skip
 *                 the wait (the caller will poll PCIe link-up via
 *                 its own mechanism).  Default suggestion:
 *                 @ref DEEPX_DXM1_DEFAULT_BOOT_US.
 * @return ALP_OK once M1_RESET is released; ALP_ERR_INVAL on a NULL @p ctx;
 *         otherwise the first failing mux/GPIO status.
 */
alp_status_t deepx_dxm1_bring_up(deepx_dxm1_t *ctx, uint32_t boot_us);

/**
 * @brief Power-down: re-assert M1_RESET, drop the PCIe muxes.
 *
 * @param ctx  Initialised sequencer context.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL @p ctx; otherwise the
 *         first failing GPIO/mux status.
 */
alp_status_t deepx_dxm1_shut_down(deepx_dxm1_t *ctx);

/**
 * @brief Release the context.  Idempotent.
 *
 * Does NOT close the underlying GPIO / mux handles -- the caller retains
 * ownership.
 *
 * @param ctx  Sequencer context (may already be deinitialised, or NULL).
 */
void deepx_dxm1_deinit(deepx_dxm1_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DEEPX_DXM1_H */
