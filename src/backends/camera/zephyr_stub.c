/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/camera.h> link cleanly on every supported SoC.
 *
 * Every op returns ALP_ERR_NOT_IMPLEMENTED -- the dispatcher
 * propagates that out of the public alp_camera_* calls; apps
 * see the same "tracked stub" contract documented in
 * docs/abi-markers.md for [BACKEND-STUB] surfaces.
 *
 * Real backends (Alif MIPI CSI-2 wrapper for AEN E4/E6/E8, V2N
 * camera input via DRP-AI) land per the tracking issues below
 * with their own silicon-specific entries in
 * src/backends/camera/ at higher priority than this wildcard.
 * ISP-specific knobs (configure_isp) follow issue #21 separately
 * since the AEN Mali-C55 ISP fabric isn't wired by Zephyr's
 * portable video driver class yet.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/20
 * @par Tracking: github.com/alplabai/alp-sdk/issues/21
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "camera_ops.h"

static alp_status_t stub_open(const alp_camera_config_t  *cfg,
                              alp_camera_backend_state_t *state,
                              alp_capabilities_t         *caps_out)
{
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_start(alp_camera_backend_state_t *state)
{
	(void)state;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_stop(alp_camera_backend_state_t *state)
{
	(void)state;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t
stub_capture(alp_camera_backend_state_t *state, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	(void)state;
	(void)out;
	(void)timeout_ms;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_release(alp_camera_backend_state_t *state, alp_camera_frame_t *frame)
{
	(void)state;
	(void)frame;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_configure_isp(alp_camera_backend_state_t    *state,
                                       const alp_camera_isp_config_t *isp)
{
	(void)state;
	(void)isp;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static const alp_camera_ops_t _ops = {
	.open          = stub_open,
	.start         = stub_start,
	.stop          = stub_stop,
	.capture       = stub_capture,
	.release       = stub_release,
	.configure_isp = stub_configure_isp,
	.close         = NULL,
};

ALP_BACKEND_REGISTER(camera,
                     zephyr_stub,
                     {
                         .silicon_ref = "*",
                         .vendor      = "stub",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
