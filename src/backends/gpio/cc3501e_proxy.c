/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Alp Lab AB
 *
 * CC3501E GPIO proxy backend (gated on CONFIG_ALP_SDK_GPIO_CC3501E_PROXY).
 *
 * On an E1M-AEN target the on-module CC3501E fronts a set of E1M pads
 * (IO11/IO13/IO15..IO21 + the camera-enable LDOs).  This backend makes those
 * pads reachable through the PORTABLE <alp/gpio.h> API: alp_gpio_open(pin_id)
 * routes a pin_id listed in the board's cc3501e_gpio_routes[] table over the
 * inter-chip bridge (chips/cc3501e -> cc3501e_gpio_*), and DELEGATES every
 * other pin_id to the platform (Zephyr) GPIO driver so the Alif's own pins
 * (WIFI_EN / nRESET / LEDs / ...) keep working unchanged.
 *
 * Because gpio uses one backend per SoC (alp_backend_select picks by
 * silicon_ref + priority), this proxy registers at a HIGHER priority than the
 * "*" platform backend and fans out per-pin internally.  It is OFF by default
 * (Kconfig n) and only enabled on AEN boards that populate the route table, so
 * it cannot disturb any other target.  With an EMPTY route table (the shipped
 * weak default) or no attached bridge, every pin delegates -- behaviourally
 * identical to the platform backend alone.
 *
 * The logical IO11.. -> raw CC3501E GPIO index map lives in the board's route
 * table (filled from the SoM pad map), NOT on the wire and NOT in the CC3501E
 * firmware (which drives the raw index 1:1).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "gpio_ops.h"

/* GPIO is fast (no worker / no radio bring-up) in the CC3501E firmware, but the
 * bridge link is briefly down if a radio op overlaps; the per-request helper
 * retries on transient IO inside this budget. */
#define CC3501E_PROXY_TMO_MS 1000u

#ifndef CONFIG_ALP_SDK_MAX_GPIO_HANDLES
#define CONFIG_ALP_SDK_MAX_GPIO_HANDLES 16
#endif

/* WEAK empty route table: a board that wants proxied IOs overrides these two
 * symbols (filled from the SoM pad map).  Default = nothing routed = every pin
 * delegates to the platform driver. */
__attribute__((weak)) const cc3501e_gpio_route_t cc3501e_gpio_routes[]    = { 0 };
__attribute__((weak)) const size_t               cc3501e_gpio_route_count = 0u;

/* Live bridge handle, set by alp_gpio_cc3501e_attach().  NULL => proxied pins
 * also delegate (no bridge to talk to yet). */
static cc3501e_t *g_bridge_ctx;

alp_status_t alp_gpio_cc3501e_attach(cc3501e_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	g_bridge_ctx = ctx;
	return ALP_OK;
}

/* Per-handle side-state: either a bridge pin (raw index) or a delegated pin
 * whose real backend state lives in `inner`. */
typedef struct {
	bool                     in_use;
	bool                     is_bridge;
	uint8_t                  cc35_raw;
	alp_gpio_backend_state_t inner; /* delegated platform-backend state */
} proxy_side_t;

static proxy_side_t _sides[CONFIG_ALP_SDK_MAX_GPIO_HANDLES];

static proxy_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES; ++i) {
		if (!_sides[i].in_use) {
			memset(&_sides[i], 0, sizeof(_sides[i]));
			_sides[i].in_use = true;
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(proxy_side_t *s)
{
	s->in_use = false;
}

/* Look up a portable pin_id in the board route table.  Returns true + the raw
 * CC3501E GPIO index when the pin is proxied. */
static bool route_lookup(uint32_t pin_id, uint8_t *raw_out)
{
	for (size_t i = 0; i < cc3501e_gpio_route_count; ++i) {
		if (cc3501e_gpio_routes[i].pin_id == pin_id) {
			*raw_out = cc3501e_gpio_routes[i].cc35_gpio;
			return true;
		}
	}
	return false;
}

static alp_status_t
px_open(uint32_t pin_id, alp_gpio_backend_state_t *state, alp_capabilities_t *caps)
{
	proxy_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	uint8_t raw = 0u;
	if (g_bridge_ctx != NULL && route_lookup(pin_id, &raw)) {
		/* Proxied pin: the bridge owns it. */
		s->is_bridge   = true;
		s->cc35_raw    = raw;
		state->be_data = s;
		state->pin_id  = pin_id;
		return ALP_OK;
	}

	/* Not proxied (or no bridge attached): delegate to the platform driver. */
	const alp_gpio_ops_t *z  = alp_z_gpio_ops();
	alp_status_t          rc = z->open(pin_id, &s->inner, caps);
	if (rc != ALP_OK) {
		_free_side(s);
		return rc;
	}
	s->is_bridge   = false;
	state->be_data = s;
	state->pin_id  = pin_id;
	return ALP_OK;
}

static alp_status_t
px_configure(alp_gpio_backend_state_t *state, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		/* Portable dir/pull enums share values with the protocol enums. */
		return cc3501e_gpio_configure(g_bridge_ctx,
		                              s->cc35_raw,
		                              (alp_cc3501e_gpio_direction_t)dir,
		                              (alp_cc3501e_gpio_pull_t)pull,
		                              CC3501E_PROXY_TMO_MS);
	}
	return alp_z_gpio_ops()->configure(&s->inner, dir, pull);
}

static alp_status_t px_write(alp_gpio_backend_state_t *state, bool level)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		return cc3501e_gpio_write(g_bridge_ctx, s->cc35_raw, level, CC3501E_PROXY_TMO_MS);
	}
	return alp_z_gpio_ops()->write(&s->inner, level);
}

static alp_status_t px_read(alp_gpio_backend_state_t *state, bool *level)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		return cc3501e_gpio_read(g_bridge_ctx, s->cc35_raw, level, CC3501E_PROXY_TMO_MS);
	}
	return alp_z_gpio_ops()->read(&s->inner, level);
}

static alp_status_t
px_enable_irq(alp_gpio_backend_state_t *state, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		/* The 3-wire bridge has no slave->master attention line this rev, so an
		 * edge on a proxied pin cannot invoke the host callback.  Report
		 * NOSUPPORT rather than arm an IRQ that never fires.  (The firmware HAL
		 * still latches the edge for the next-rev host-IRQ / poll path.) */
		(void)edge;
		(void)cb;
		(void)user;
		return ALP_ERR_NOSUPPORT;
	}
	return alp_z_gpio_ops()->enable_irq(&s->inner, edge, cb, user);
}

static alp_status_t px_disable_irq(alp_gpio_backend_state_t *state)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		return cc3501e_gpio_set_interrupt(
		    g_bridge_ctx, s->cc35_raw, ALP_CC3501E_GPIO_EDGE_NONE, false, CC3501E_PROXY_TMO_MS);
	}
	return alp_z_gpio_ops()->disable_irq(&s->inner);
}

static void px_close(alp_gpio_backend_state_t *state)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return;
	if (!s->is_bridge) {
		alp_z_gpio_ops()->close(&s->inner);
	}
	_free_side(s);
	state->be_data = NULL;
}

static const alp_gpio_ops_t _ops = {
	.open        = px_open,
	.configure   = px_configure,
	.write       = px_write,
	.read        = px_read,
	.enable_irq  = px_enable_irq,
	.disable_irq = px_disable_irq,
	.close       = px_close,
};

/* Higher priority than the "*" platform backend so it wins on the AEN target
 * where this file is compiled (Kconfig-gated); fans out per-pin internally. */
ALP_BACKEND_REGISTER(gpio,
                     cc3501e_proxy,
                     {
                         .silicon_ref = "*",
                         .vendor      = "ti-cc3501e",
                         .base_caps   = 0u,
                         .priority    = 200,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
