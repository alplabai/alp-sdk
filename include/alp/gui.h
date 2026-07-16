/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gui.h
 * @brief Alp SDK GUI library — LVGL re-export with ALP defaults.
 *
 * This header pulls in upstream LVGL (when ALP_HAS_LVGL is set by the
 * build) and exposes ALP-specific helpers for binding LVGL to
 * alp_display_t and alp_input_*.  No custom widgets ship.
 *
 * Including <alp/gui.h> requires the LVGL package to be on the
 * include path; this is a build option, not bundled source.  On
 * Zephyr, set CONFIG_LVGL=y -- CONFIG_ALP_SDK_HAS_LVGL then defaults
 * on and the build defines ALP_HAS_LVGL automatically (see
 * zephyr/kconfigs/hw-info.kconfig); on plain-CMake / Yocto builds set
 * -DALP_HAS_LVGL=ON and supply the LVGL include path yourself.
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
#include "lvgl.h"
#endif

/**
 * @brief Bind an alp_display_t to LVGL's display driver.
 *
 * Allocates an lv_display_t (LVGL v9) sized to @p display's reported
 * geometry, wires its flush callback to @ref alp_display_blit, and
 * allocates a persistent partial-refresh draw buffer (height set by
 * CONFIG_ALP_GUI_LVGL_BUF_LINES on Zephyr, default 16 lines elsewhere).
 * Call @c lv_init() once before attaching any display.
 *
 * @param[in] display  Open handle from @ref alp_display_open.
 *                     Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL display) /
 *         ALP_ERR_NOSUPPORT (build not compiled with ALP_HAS_LVGL, or
 *         @p display's pixel format has no LVGL v9 equivalent) /
 *         ALP_ERR_NOMEM (lv_display_create or the draw-buffer
 *         allocation failed) / any error @ref alp_display_get_caps
 *         propagates.
 */
alp_status_t alp_gui_lvgl_attach(alp_display_t *display);

#ifdef __cplusplus
}
#endif

#endif /* ALP_GUI_H */
