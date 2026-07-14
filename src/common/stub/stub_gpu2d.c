/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU2D NOSUPPORT stubs -- <alp/gpu2d.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_gpu2d_*` symbol not provided by a vendor backend or the
 * portable sw_fallback dispatcher.
 */

#include <stdint.h>

#include "alp/gpu2d.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

/* Muted when the OS backend compiles the real class dispatcher
 * (src/gpu2d_dispatch.c + the portable sw_fallback) -- the Yocto
 * build does, so Linux apps get the REAL CPU fill/blit/blend
 * instead of NOSUPPORT (same #33 migration pattern as rtc/wdt/
 * can/pwm/adc/i2s/counter above). */
#if !defined(ALP_VENDOR_OVERRIDES_GPU2D)
alp_gpu2d_t *alp_gpu2d_open(void)
{
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t               *g,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   x,
                                 uint32_t                   y,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 uint32_t                   argb_color)
{
	(void)g;
	(void)dst;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)argb_color;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpu2d_blit(alp_gpu2d_t               *g,
                            const alp_gpu2d_surface_t *src,
                            uint32_t                   sx,
                            uint32_t                   sy,
                            const alp_gpu2d_surface_t *dst,
                            uint32_t                   dx,
                            uint32_t                   dy,
                            uint32_t                   w,
                            uint32_t                   h)
{
	(void)g;
	(void)src;
	(void)sx;
	(void)sy;
	(void)dst;
	(void)dx;
	(void)dy;
	(void)w;
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpu2d_blend(alp_gpu2d_t               *g,
                             const alp_gpu2d_surface_t *src,
                             uint32_t                   sx,
                             uint32_t                   sy,
                             const alp_gpu2d_surface_t *dst,
                             uint32_t                   dx,
                             uint32_t                   dy,
                             uint32_t                   w,
                             uint32_t                   h,
                             alp_gpu2d_blend_mode_t     mode)
{
	(void)g;
	(void)src;
	(void)sx;
	(void)sy;
	(void)dst;
	(void)dx;
	(void)dy;
	(void)w;
	(void)h;
	(void)mode;
	return ALP_ERR_NOSUPPORT;
}
void alp_gpu2d_close(alp_gpu2d_t *g)
{
	(void)g;
}
const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *g)
{
	(void)g;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_GPU2D */
