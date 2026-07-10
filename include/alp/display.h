/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file display.h
 * @brief Alp SDK display abstraction.
 *
 * Portable surface only.  On Zephyr builds that link
 * CONFIG_DISPLAY, ops are served by the real `display_*`
 * driver-class wrapper (src/backends/display/zephyr_drv.c) -- any
 * panel with an upstream Zephyr display driver resolves via the
 * `alp-display0..3` devicetree aliases.  Elsewhere the priority-0
 * NOT_IMPLEMENTED stub (zephyr_stub.c) keeps the surface linkable.
 * Still tracked by issue #23:
 *   - the V2N DSI / parallel-RGB framebuffer path,
 *   - the Alif LCD-IF path,
 *   - alp_gui_lvgl_attach() for LVGL flush integration
 *     (<alp/gui.h>).
 *

 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.3 surface; Zephyr driver-class backend since v0.9
 *      (code-complete, native_sim-tested, no silicon run yet).
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_DISPLAY_H
#define ALP_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alp_display alp_display_t;

/* alp_pixfmt_t is defined in <alp/peripheral.h> so it can be
 * shared with the camera surface without a forward dependency. */

typedef struct {
	uint32_t display_id; /**< Studio-resolved display instance. */
} alp_display_config_t;

/**
 * @brief Default-initialize an @ref alp_display_config_t for display @p id.
 *
 * The display config has no tunable fields beyond its identity, so the
 * default simply names the instance: @code alp_display_config_t cfg =
 * ALP_DISPLAY_CONFIG_DEFAULT(0); @endcode
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_DISPLAY_CONFIG_DEFAULT(id) ((alp_display_config_t){ .display_id = (id) })

typedef struct {
	uint16_t     width;
	uint16_t     height;
	alp_pixfmt_t format;
} alp_display_caps_t;

/**
 * @brief Open the display handle defined by @p cfg.
 *
 * @param[in] cfg  Display id.  Must be non-NULL.
 *
 * @return Open handle on success; NULL with @ref alp_last_error
 *         set to @ref ALP_ERR_INVAL / @ref ALP_ERR_NOT_READY
 *         (display id not DT-resolved) / @ref ALP_ERR_NOSUPPORT.
 */
alp_display_t *alp_display_open(const alp_display_config_t *cfg);

/**
 * @brief Read the display's static capabilities (geometry + pixel format).
 *
 * @param[in]  d    Handle from @ref alp_display_open.
 * @param[out] out  Filled with width / height / format.
 *                  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_get_caps(alp_display_t *d, alp_display_caps_t *out);

/**
 * @brief Push a rectangular framebuffer region.  `pixels` size is implied by w*h*format.
 *
 * @param[in] d       Handle from @ref alp_display_open.
 * @param[in] x, y    Top-left of the destination rect (pixels).
 * @param[in] w, h    Size of the rect (pixels).
 * @param[in] pixels  Source framebuffer pointer; must be non-NULL.
 *                    Byte size = `w * h * bytes-per-pixel-for-format`.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE
 *         (rect outside display caps) / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_blit(
    alp_display_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *pixels);

/**
 * @brief Clear the framebuffer to the background colour.
 *
 * @param[in] d  Handle from @ref alp_display_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_clear(alp_display_t *d);

/**
 * @brief Release the display handle.  Idempotent on NULL.
 *
 * @param[in] d  Handle from @ref alp_display_open, or NULL.
 */
void alp_display_close(alp_display_t *d);

/**
 * @brief Query the capabilities of an opened display handle.
 *
 * @param d  Handle from @ref alp_display_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p d is NULL.
 */
const alp_capabilities_t *alp_display_capabilities(const alp_display_t *d);

#ifdef __cplusplus
}
#endif

#endif /* ALP_DISPLAY_H */
