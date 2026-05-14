/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gui.h
 * @brief ALP SDK GUI library — LVGL re-export with ALP defaults.
 *
 * v0.1: this header pulls in upstream LVGL (when ALP_HAS_LVGL is set
 * by the build) and exposes ALP-specific helpers for binding LVGL
 * to alp_display_t and alp_input_*.  No custom widgets ship in v0.1.
 *
 * Including <alp/gui.h> requires the LVGL package to be on the
 * include path; this is a build option, not bundled source.
 *

 * @par ABI status: [ABI-STABLE]
 *      v0.2 LVGL re-export shim.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_GUI_H
#define ALP_GUI_H

#include "alp/display.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ALP_HAS_LVGL
/* The upstream LVGL umbrella header.  Configured via lv_conf.h
 * shipped under cmake/ — see docs/architecture.md. */
#  include "lvgl.h"
#endif

/**
 * @brief Bind an alp_display_t to LVGL's display driver.
 *
 * Allocates an lv_display_t (LVGL >= 9) or lv_disp_drv_t
 * (LVGL 8) wired to alp_display_blit().
 *
 * Returns ALP_ERR_NOSUPPORT when the build was not compiled with
 * ALP_HAS_LVGL.
 */
alp_status_t alp_gui_lvgl_attach(alp_display_t *display);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_GUI_H */
