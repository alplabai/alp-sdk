/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera (+ ISP configuration) NOSUPPORT stubs -- <alp/camera.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_camera_*` symbol not provided by a vendor
 * backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/camera.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_CAMERA)
alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_camera_start(alp_camera_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_stop(alp_camera_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_capture(alp_camera_t *c, alp_camera_frame_t *o, uint32_t t)
{
	(void)c;
	(void)o;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *f)
{
	(void)c;
	(void)f;
	return ALP_ERR_NOSUPPORT;
}
void alp_camera_close(alp_camera_t *c)
{
	(void)c;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAMERA */

/* ------------------------------------------------------------------ */
/* Camera ISP (alp/camera.h v0.5 extension)                            */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_CAMERA)
alp_status_t alp_camera_configure_isp(alp_camera_t *c, const alp_camera_isp_config_t *isp)
{
	if (isp == NULL) return ALP_ERR_INVAL;
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAMERA */
