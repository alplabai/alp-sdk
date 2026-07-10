/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gfx_compat -- a tiny, maintainer-written 2D fill/blit shim.
 *
 * Unlike the other 14 libraries in `docs/recommended-libraries.md`'s
 * Tier 1 list, gfx_compat is NOT a fetched third-party library --
 * `west.yml` documents it as shipping in-tree (see the `gfx_compat`
 * comment in the `extras-tier1` block), and this directory is that
 * in-tree source. It exists to give `examples/display/gfx-compat-blit`
 * (and any small app that wants raw RGB565 fill/blit without pulling
 * in a full display library) a natural, wrapper-free API to call
 * directly -- `#include "gfx_compat.h"`, no `<alp/...>` wrapper indirection.
 *
 * This header declares only the pure-C software fallback
 * (CONFIG_ALP_GFX_COMPAT_SW, see zephyr/Kconfig.alp-libraries).
 * Hardware-accelerated backends (Alif GPU2D / generic DMA2D / SPI-DMA
 * push) are metadata-tracked in
 * metadata/library-profiles/gfx_compat/hw-backends.yaml as `status:
 * planned` -- they are not yet implemented, and this header's two
 * functions are the only entry points that exist today.
 */

#ifndef ALP_SDK_LIB_GFX_COMPAT_H_
#define ALP_SDK_LIB_GFX_COMPAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill a w x h RGB565 buffer with a solid colour.
 * @param[out] buf   Destination buffer, at least @p w * @p h pixels
 *                    (row-major, no stride padding).
 * @param[in]  w     Width in pixels.
 * @param[in]  h     Height in pixels.
 * @param[in]  color RGB565 fill colour.
 *
 * Pure CPU, no allocation, no hardware dependency -- the
 * CONFIG_ALP_GFX_COMPAT_SW fallback. @p buf must hold at least
 * `w * h` `uint16_t` elements; a NULL @p buf or non-positive @p w /
 * @p h is a no-op (defensive, not an error return -- this shim has
 * no status type, matching its "tiny" scope).
 */
void gfx_compat_fill(uint16_t *buf, int w, int h, uint16_t color);

/**
 * @brief Copy a w x h RGB565 rect from @p src into @p dst.
 * @param[out] dst Destination buffer, at least @p w * @p h pixels
 *                  (row-major, no stride padding).
 * @param[in]  src Source buffer, at least @p w * @p h pixels
 *                 (row-major, no stride padding).
 * @param[in]  w   Width in pixels.
 * @param[in]  h   Height in pixels.
 *
 * Straight top-left-origin copy of a full @p w x @p h rect -- no
 * per-surface stride, no sub-rect offset, no clipping (the caller
 * slices the sub-rect into contiguous @p src / @p dst buffers before
 * calling, e.g. by pointer arithmetic into a larger framebuffer with
 * a matching stride copy loop). A NULL @p src / @p dst or
 * non-positive @p w / @p h is a no-op.
 */
void gfx_compat_blit(uint16_t *dst, const uint16_t *src, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_LIB_GFX_COMPAT_H_ */
