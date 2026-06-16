/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/display.h> link cleanly on every supported SoC.
 *
 * Every op returns ALP_ERR_NOT_IMPLEMENTED -- the dispatcher
 * propagates that out of the public alp_display_* calls; apps
 * see the same "tracked stub" contract documented in
 * docs/abi-markers.md for [BACKEND-STUB] surfaces.
 *
 * Real backends (Zephyr `display_*` driver-class wrapper for
 * SSD1306 / ILI9341 / ST7789 / similar parts on AEN, V2N DSI and
 * parallel-RGB framebuffer paths) land per the tracking issue
 * below with their own silicon-specific entries in
 * src/backends/display/ at higher priority than this wildcard.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/23
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>

#include "display_ops.h"

static alp_status_t stub_open(const alp_display_config_t  *cfg,
                              alp_display_backend_state_t *state,
                              alp_capabilities_t          *caps_out)
{
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_get_caps(alp_display_backend_state_t *state, alp_display_caps_t *out)
{
	(void)state;
	(void)out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_blit(alp_display_backend_state_t *state,
                              uint16_t                     x,
                              uint16_t                     y,
                              uint16_t                     w,
                              uint16_t                     h,
                              const void                  *pixels)
{
	(void)state;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)pixels;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_clear(alp_display_backend_state_t *state)
{
	(void)state;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static const alp_display_ops_t _ops = {
	.open     = stub_open,
	.get_caps = stub_get_caps,
	.blit     = stub_blit,
	.clear    = stub_clear,
	.close    = NULL,
};

ALP_BACKEND_REGISTER(display,
                     zephyr_stub,
                     {
                         .silicon_ref = "*",
                         .vendor      = "stub",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
