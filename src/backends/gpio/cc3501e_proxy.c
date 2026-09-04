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
 * (WIFI_EN / nRESET / LEDs / ...) keep working unchanged -- EXCEPT a pin_id
 * listed in the board's cc3501e_gpio_unrouted[] list, which is refused
 * outright with ALP_ERR_NOSUPPORT: that list names E1M pads that are
 * physically open on the running hardware revision (reach neither the
 * CC3501E nor the Alif SoC, e.g. AEN r2's IO21), so delegating them would
 * silently open and drive a pin that goes nowhere (issue #1854).
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
#include "alp_slot_claim.h"

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h>      /* alp_hw_info_read(), alp_hw_info_t, ALP_OK */
#include "hw_info_manifest.h" /* alp_hw_info_build_hw_rev_mismatch() -- internal, issue #1859 */
#endif

/* GPIO is fast (no worker / no radio bring-up) in the CC3501E firmware, but the
 * bridge link is briefly down if a radio op overlaps; the per-request helper
 * retries on transient IO inside this budget. */
#define CC3501E_PROXY_TMO_MS 1000u

#ifndef CONFIG_ALP_SDK_MAX_GPIO_HANDLES
#define CONFIG_ALP_SDK_MAX_GPIO_HANDLES 16
#endif

/* Board-provided route table: a board that wants proxied IOs overrides
 * cc3501e_gpio_routes[] / cc3501e_gpio_route_count (filled from the SoM pad
 * map); default = nothing routed = every pin delegates to the platform
 * driver.  <alp/chips/cc3501e/gpio.h> (pulled in above) declares both
 * `extern`; the WEAK empty default now lives in its OWN translation unit,
 * cc3501e_proxy_routes_weak.c -- NOT here.  Defining a weak `const` in the
 * same TU that reads it let the compiler see its own zero initializer and
 * fold cc3501e_gpio_route_count to 0 at -Os, silently eliminating the loop
 * below and making a board's strong override in another TU dead code
 * (issue #1860).  Moving the weak default out means this TU only ever sees
 * an `extern` declaration, so route_lookup() below always compiles a real
 * load + call through the linked symbol, strong or weak. */

/* WEAK empty unrouted list: a board overrides these two symbols (filled from
 * the SoM pad map's `dispatch: unrouted` entries, e.g. AEN r2's IO21 -- issue
 * #1854) to name pin_ids that are physically open on this hardware revision.
 * Checked in px_open() BEFORE the route/delegate decision below, so it is the
 * single chokepoint every alp_gpio_open() call on this target passes through
 * -- not just the boards that happen to populate cc3501e_gpio_routes[]. */

/* Live bridge handle, set by alp_gpio_cc3501e_attach().  NULL => proxied pins
 * also delegate (no bridge to talk to yet). */
static cc3501e_t *g_bridge_ctx;

#if defined(CONFIG_ALP_SDK_HW_INFO)
/* Cached ONCE in alp_gpio_cc3501e_attach() (not re-read per alp_gpio_open(),
 * which would put an EEPROM I2C transaction on every proxied pin open).
 * True when the live module's hw_rev disagrees with CONFIG_ALP_SDK_SOM_HW_REV
 * -- the rev this build's cc3501e_gpio_routes[] table was generated for
 * (scripts/gen_cc3501e_gpio_routes.py, issue #1859).  Mirrors #1853's boot
 * banner check (src/zephyr/alp_banner.c); this is the "stronger guard" that
 * fix deferred: the AEN family moves IO8/IO10/IO21 between the Alif and the
 * CC3501E across hw_rev, so a route table compiled for the wrong revision
 * would otherwise silently drive a different physical pin than the caller
 * asked for.  Stays false (never refuses) when the manifest can't be read
 * (NOT_PROVISIONED / no EEPROM bus wired / NOSUPPORT) -- same floor as the
 * banner check: a factory-fresh or EEPROM-less module never trips this. */
static bool g_hw_rev_mismatch;
#endif

alp_status_t alp_gpio_cc3501e_attach(cc3501e_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	g_bridge_ctx = ctx;
#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;
	if (alp_hw_info_read(&info) == ALP_OK) {
		g_hw_rev_mismatch = alp_hw_info_build_hw_rev_mismatch(&info, CONFIG_ALP_SDK_SOM_HW_REV);
	}
#endif
	return ALP_OK;
}

/* Per-handle side-state: either a bridge pin (raw index) or a delegated pin
 * whose real backend state lives in `inner`.  in_use is the LAST member
 * (issue #1115 round-2 dev review, mirrors dsp/sw_fallback.c's struct
 * dsp_be): the atomic claimant in _alloc_side() below memsets only the
 * bytes ahead of it, so the claim is never transiently undone. */
typedef struct {
	bool                     is_bridge;
	uint8_t                  cc35_raw;
	alp_gpio_backend_state_t inner; /* delegated platform-backend state */
	bool                     in_use;
} proxy_side_t;

static proxy_side_t _sides[CONFIG_ALP_SDK_MAX_GPIO_HANDLES];

/* issue #1115 round-2 dev review: claim atomically instead of the
 * previous plain check-then-set scan. */
static proxy_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES; ++i) {
		if (alp_slot_try_claim(&_sides[i].in_use)) {
			memset(&_sides[i], 0, offsetof(proxy_side_t, in_use));
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(proxy_side_t *s)
{
	alp_slot_release(&s->in_use);
}

/* Look up a portable pin_id in the board route table.  Returns true + the raw
 * CC3501E GPIO index when the pin is proxied.  Returns false unconditionally
 * on a detected hw_rev mismatch (issue #1859): px_open()'s caller then
 * delegates to the platform driver instead of driving the wrong physical
 * chip, the same fallback an un-populated route table already gets. */
static bool route_lookup(uint32_t pin_id, uint8_t *raw_out)
{
#if defined(CONFIG_ALP_SDK_HW_INFO)
	if (g_hw_rev_mismatch) {
		return false;
	}
#endif
	for (size_t i = 0; i < cc3501e_gpio_route_count; ++i) {
		if (cc3501e_gpio_routes[i].pin_id == pin_id) {
			*raw_out = cc3501e_gpio_routes[i].cc35_gpio;
			return true;
		}
	}
	return false;
}

/* Look up a portable pin_id in the board's unrouted list (issue #1854). */
static bool is_unrouted(uint32_t pin_id)
{
	for (size_t i = 0; i < cc3501e_gpio_unrouted_count; ++i) {
		if (cc3501e_gpio_unrouted[i] == pin_id) return true;
	}
	return false;
}

static alp_status_t
px_open(uint32_t pin_id, alp_gpio_backend_state_t *state, alp_capabilities_t *caps)
{
	/* Refuse a pin the board has named as physically open on this hardware
	 * revision BEFORE the route/delegate decision below -- this is every
	 * alp_gpio_open() call's single path through the AEN GPIO proxy, so
	 * checking here (not in one example / one caller) covers every app
	 * (issue #1854). */
	if (is_unrouted(pin_id)) return ALP_ERR_NOSUPPORT;

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

	/* Not proxied (or no bridge attached): delegate to the platform driver.
	 *
	 * Name the owning handle explicitly.  The platform backend needs it to
	 * dispatch interrupts (the dispatcher stashes the callback on the handle,
	 * and the ISR thunk calls it from there), and it cannot derive it here:
	 * `&s->inner` is nested in this backend's per-handle sidecar, not in a
	 * struct alp_gpio, so the CONTAINER_OF its own open() would apply lands
	 * outside any handle and the thunk would call a bogus pointer from
	 * interrupt context (issue #1618).  `state` IS &handle->state -- the
	 * dispatcher passed it -- so it names the owner without this file needing
	 * to know the handle layout. */
	alp_status_t rc = alp_z_gpio_open_owned(pin_id, &s->inner, caps, state);
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
