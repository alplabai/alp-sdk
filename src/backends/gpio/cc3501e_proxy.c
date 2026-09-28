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
 * A THIRD list, cc3501e_gpio_rev_dependent[] -- SDK-owned, not a board list
 * like the two above -- names E1M pads whose
 * target chip (CC3501E vs the Alif SoC vs unrouted) metadata/e1m_modules/
 * aen/hw-revisions.yaml `pad_route_overrides:` moves between AEN hw_revs --
 * IO8/IO10/IO21 today.  A route table is always compiled for ONE hw_rev; on
 * any OTHER hw_rev, opening one of these pins would silently drive a
 * DIFFERENT physical net than the caller asked for -- on r1, IO21 reaches
 * CC3501E GPIO_30, which is tied to +3V3 through R198 and a fitted P18
 * jumper (a contention hazard, not just a functional miss).  px_open()
 * refuses a pin on this list with ALP_ERR_NOSUPPORT UNLESS a CRC-valid
 * identity-EEPROM manifest (<alp/hw_info.h>) confirms the running module's
 * hw_rev equals CONFIG_ALP_SDK_SOM_HW_REV -- the hw_rev the board's route
 * table (and this list) were built for.  FAILS CLOSED: a missing,
 * unprovisioned or corrupt manifest refuses the pin, replacing the old
 * fail-open, all-or-nothing g_hw_rev_mismatch guard (#1859) that dropped
 * every proxied pin -- including revision-INDEPENDENT ones like IO20, the
 * SD mux enable -- on any hw_rev disagreement, or silently kept routing
 * when the manifest could not be read at all.  See issue #2144.
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

#include <zephyr/logging/log.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "gpio_ops.h"
#include "alp_slot_claim.h"
#include "cc3501e_proxy_internal.h"

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h> /* alp_hw_info_read(), alp_hw_info_assert_matches_build() */
#endif

LOG_MODULE_REGISTER(alp_gpio_cc3501e_proxy, CONFIG_LOG_DEFAULT_LEVEL);

/* Defined in the SDK-owned generated file src/backends/gpio/
 * cc3501e_rev_dependent_pins.c (scripts/gen_cc3501e_gpio_routes.py), NOT
 * declared in the public <alp/chips/cc3501e/gpio.h> -- unlike
 * cc3501e_gpio_routes[]/cc3501e_gpio_unrouted[] above (board-provided,
 * declared in that header), this list is identical on every AEN board and
 * has exactly one definition, always linked alongside this file
 * (zephyr/CMakeLists.txt), never overridden (issue #2144 design review). */
extern const uint32_t cc3501e_gpio_rev_dependent[];
extern const size_t   cc3501e_gpio_rev_dependent_count;

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

/* Cached ONCE in alp_gpio_cc3501e_attach() (not re-read per alp_gpio_open(),
 * which would put an EEPROM I2C transaction on every proxied pin open).
 * Declared unconditionally (not inside a CONFIG_ALP_SDK_HW_INFO guard) so a
 * build with the hw_info reader compiled OUT still has a well-defined,
 * FAIL-CLOSED value: false, "not confirmed" -- see is_rev_dependent()'s use
 * of it in px_open() below.  True only when attach() proved the running
 * module's hw_rev equals CONFIG_ALP_SDK_SOM_HW_REV, the rev this board's
 * cc3501e_gpio_routes[] table was built for (cc3501e_gpio_rev_dependent[]
 * itself is not per-rev -- it is the SDK-owned, identical-on-every-AEN-board
 * list of pins that need this check at all; see the file header)
 * (issue #2144). */
static bool g_hw_rev_confirmed_match;

#if defined(CONFIG_ALP_SDK_HW_INFO)
/* Pure decision, no I2C: does @p read_status + @p info -- an
 * alp_hw_info_read() result -- confirm the running module's hw_rev matches
 * CONFIG_ALP_SDK_SOM_HW_REV?  Split out from alp_gpio_cc3501e_attach() so a
 * native_sim test can drive every case (matching / mismatched / corrupt /
 * missing manifest) with a crafted status + alp_hw_info_t, the same
 * testability split alp_hw_info_classify_manifest() uses
 * (src/zephyr/hw_info_zephyr.c) for the manifest reader itself.
 *
 * @p read_status must be ALP_OK (a CRC-valid manifest -- alp_hw_info_read()
 * only returns ALP_OK past magic + schema_version + CRC32 validation) for a
 * match to be possible at all; alp_hw_info_assert_matches_build() then does
 * the bounded hw_rev compare, itself refusing a match against an empty
 * CONFIG_ALP_SDK_SOM_HW_REV (a build that never recorded which hw_rev its
 * route table targets fails closed too, not "nothing to compare"). */
bool cc3501e_proxy_hw_rev_confirmed_match(alp_status_t read_status, const alp_hw_info_t *info)
{
	return read_status == ALP_OK &&
	       alp_hw_info_assert_matches_build(info, NULL, CONFIG_ALP_SDK_SOM_HW_REV) == ALP_OK;
}
#endif

#if defined(CONFIG_ZTEST)
/* Test-only hook: force the cached decision without a real EEPROM read.
 * native_sim has no I2C EEPROM to back alp_hw_info_read(), so a ztest
 * exercising px_open()'s per-pin gate (as opposed to
 * cc3501e_proxy_hw_rev_confirmed_match()'s own logic, which it can call
 * directly) has no other way to reach the "confirmed" state.  Never called
 * by production code -- compiled only into CONFIG_ZTEST=y test images. */
void cc3501e_proxy_test_force_hw_rev_confirmed_match(bool confirmed)
{
	g_hw_rev_confirmed_match = confirmed;
}

/* Test-only hook: arm a canned alp_hw_info_read() result for the next
 * alp_gpio_cc3501e_attach() call.  See cc3501e_proxy_internal.h for why
 * this (not the force hook above) is the seam a POSITIVE attach() test
 * needs -- it swaps only alp_hw_info_read()'s source, so attach()'s own
 * cc3501e_proxy_hw_rev_confirmed_match() call and cache assignment still
 * run for real. */
static bool          g_test_hw_info_armed;
static alp_status_t  g_test_hw_info_status;
static alp_hw_info_t g_test_hw_info_info;

void cc3501e_proxy_test_inject_hw_info_read(alp_status_t status, const alp_hw_info_t *info)
{
	g_test_hw_info_armed  = true;
	g_test_hw_info_status = status;
	if (info != NULL) {
		g_test_hw_info_info = *info;
	} else {
		memset(&g_test_hw_info_info, 0, sizeof(g_test_hw_info_info));
	}
}

/* Test-only teardown: see cc3501e_proxy_internal.h. */
void cc3501e_proxy_test_reset_bridge_ctx(void)
{
	g_bridge_ctx = NULL;
}
#endif

alp_status_t alp_gpio_cc3501e_attach(cc3501e_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	g_bridge_ctx = ctx;
#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;
	alp_status_t  rc;
#if defined(CONFIG_ZTEST)
	/* One-shot test injection (see cc3501e_proxy_test_inject_hw_info_read()
	 * above): lets a positive attach() test prove this function's real
	 * body -- not the force hook -- turns a confirmed manifest into
	 * g_hw_rev_confirmed_match=true, without native_sim needing a real I2C
	 * EEPROM. Disarmed immediately so a later attach() without re-arming
	 * falls through to the real read below. */
	if (g_test_hw_info_armed) {
		g_test_hw_info_armed = false;
		rc                   = g_test_hw_info_status;
		info                 = g_test_hw_info_info;
	} else
#endif
	{
		rc = alp_hw_info_read(&info);
	}
	g_hw_rev_confirmed_match = cc3501e_proxy_hw_rev_confirmed_match(rc, &info);
	/* Diagnostic (issue #2144 review): attach() runs once at bring-up, so
	 * every revision-dependent pin's later refusal traces back to THIS
	 * decision -- log it once here instead of leaving a bench engineer to
	 * infer "not confirmed" from a bare ALP_ERR_NOSUPPORT on IO8. rc==ALP_OK
	 * means info.som_hw_rev is a CRC-valid read; any other rc leaves it
	 * zero-filled (alp_hw_info_read()'s documented contract), so print
	 * "(unread)" rather than a misleadingly empty string. */
	if (!g_hw_rev_confirmed_match) {
		LOG_WRN_ONCE("hw_rev not confirmed: alp_hw_info_read()=%d, manifest "
		             "som_hw_rev=\"%s\", build CONFIG_ALP_SDK_SOM_HW_REV=\"%s\" -- "
		             "revision-dependent E1M pins (IO8/IO10/IO21) will refuse "
		             "ALP_ERR_NOSUPPORT",
		             (int)rc,
		             rc == ALP_OK ? info.som_hw_rev : "(unread)",
		             CONFIG_ALP_SDK_SOM_HW_REV);
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
	/* ctx->link_epoch (issue #2126) at the last successful px_configure()
	 * on this handle. The bridge reboots on recovery, taking every
	 * firmware GPIO direction/pull setting with it -- a proxied pin's
	 * REAL config is gone the moment link_epoch changes, even though this
	 * side-state and the caller's handle both still look valid. Checked
	 * against g_bridge_ctx->link_epoch by px_write()/px_read()/
	 * px_disable_irq() below; a mismatch means "reconfigure before using
	 * this pin again", not "this handle is closed" -- px_configure() may
	 * still be called on it. */
	uint8_t                  cfg_epoch;
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

/* Look up a portable pin_id in the board's unrouted list (issue #1854). */
static bool is_unrouted(uint32_t pin_id)
{
	for (size_t i = 0; i < cc3501e_gpio_unrouted_count; ++i) {
		if (cc3501e_gpio_unrouted[i] == pin_id) return true;
	}
	return false;
}

/* Look up a portable pin_id in the board's revision-dependent list (issue
 * #2144) -- an E1M pad whose target chip metadata/e1m_modules/aen/
 * hw-revisions.yaml `pad_route_overrides:` moves across hw_revs, so a route
 * table built for one hw_rev is only correct on that ONE hw_rev. */
static bool is_rev_dependent(uint32_t pin_id)
{
	for (size_t i = 0; i < cc3501e_gpio_rev_dependent_count; ++i) {
		if (cc3501e_gpio_rev_dependent[i] == pin_id) return true;
	}
	return false;
}

/* One-shot-per-pin diagnostic (issue #2144 review): px_open() also returns
 * ALP_ERR_NOSUPPORT for an UNROUTED pin (the is_unrouted() check in px_open()
 * below), so a bare error code
 * alone doesn't tell a bench engineer which guard fired.  Logs at most once
 * per DISTINCT revision-dependent pin_id -- not once ever (that would miss
 * every pin after the first this process happens to refuse) and not once
 * per call (a caller retrying alp_gpio_open() on the same refused pin would
 * otherwise flood the log).
 * ponytail: a 32-bit mask caps individual dedup at the first 32 entries of
 * cc3501e_gpio_rev_dependent[] (today: 3); pin #33 in a future hw-revisions.yaml
 * would just log on every refusal instead of once -- widen to a wider
 * bitset if that list ever grows that large. */
static uint32_t g_rev_dependent_warned_mask;

static void warn_rev_dependent_refused(uint32_t pin_id)
{
	for (size_t i = 0; i < cc3501e_gpio_rev_dependent_count && i < 32u; ++i) {
		if (cc3501e_gpio_rev_dependent[i] != pin_id) continue;
		if (g_rev_dependent_warned_mask & (1u << i)) return;
		g_rev_dependent_warned_mask |= (1u << i);
		LOG_WRN("alp_gpio_open(pin_id=%u) refused by the #2144 revision guard "
		        "(not the #1854 unrouted-pad guard): manifest hw_rev unconfirmed",
		        pin_id);
		return;
	}
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

	/* Refuse a REVISION-DEPENDENT pin PER PIN, not all-or-nothing, unless
	 * a CRC-valid manifest confirmed this build's hw_rev is the one its
	 * route table was compiled for.  FAILS CLOSED -- unlike the #1859
	 * guard this replaces, an unreadable/missing manifest refuses here
	 * too, and a revision-INDEPENDENT pin (e.g. IO20, the SD mux enable)
	 * never reaches this check at all, so it keeps routing/delegating
	 * exactly as before regardless of the manifest (issue #2144). */
	if (is_rev_dependent(pin_id) && !g_hw_rev_confirmed_match) {
		warn_rev_dependent_refused(pin_id);
		return ALP_ERR_NOSUPPORT;
	}

	proxy_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	uint8_t raw = 0u;
	if (g_bridge_ctx != NULL && route_lookup(pin_id, &raw)) {
		/* Proxied pin: the bridge owns it. */
		s->is_bridge   = true;
		s->cc35_raw    = raw;
		/* #2126 review (minor): stamp the link epoch the handle was
		 * opened under. _alloc_side() zeroes it, and epoch 0 is only
		 * ever current before the first recovery -- so without this a
		 * pin opened AFTER a recovery answered ALP_ERR_NOT_READY on
		 * every op until the caller happened to call
		 * alp_gpio_configure(), while the identical sequence on a ctx
		 * that had never recovered worked. The staleness rule is "the
		 * firmware rebooted since this handle last agreed with it";
		 * a handle opened now agrees with it now. */
		s->cfg_epoch   = g_bridge_ctx->link_epoch;
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
		alp_status_t rc = cc3501e_gpio_configure(g_bridge_ctx,
		                                         s->cc35_raw,
		                                         (alp_cc3501e_gpio_direction_t)dir,
		                                         (alp_cc3501e_gpio_pull_t)pull,
		                                         CC3501E_PROXY_TMO_MS);
		/* #2126 review: this call itself may as well be the reconfigure a
		 * stale epoch below demands -- stamp it on success, whether or not
		 * this is the pin's first configure. */
		if (rc == ALP_OK && g_bridge_ctx != NULL) s->cfg_epoch = g_bridge_ctx->link_epoch;
		return rc;
	}
	return alp_z_gpio_ops()->configure(&s->inner, dir, pull);
}

/* #2126 review: true when @p s's bridge configuration is STALE -- the
 * firmware rebooted (link_epoch changed) since px_configure() last set it
 * on this handle, so the direction/pull it thinks it has is gone. Every
 * bridge op below except px_configure() itself (which IS the reconfigure)
 * refuses with ALP_ERR_NOT_READY rather than silently driving/reading a pin
 * under a config the firmware no longer has. */
static bool px_bridge_cfg_stale(const proxy_side_t *s)
{
	return g_bridge_ctx == NULL || s->cfg_epoch != g_bridge_ctx->link_epoch;
}

static alp_status_t px_write(alp_gpio_backend_state_t *state, bool level)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		if (px_bridge_cfg_stale(s)) return ALP_ERR_NOT_READY;
		return cc3501e_gpio_write(g_bridge_ctx, s->cc35_raw, level, CC3501E_PROXY_TMO_MS);
	}
	return alp_z_gpio_ops()->write(&s->inner, level);
}

static alp_status_t px_read(alp_gpio_backend_state_t *state, bool *level)
{
	proxy_side_t *s = (proxy_side_t *)state->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	if (s->is_bridge) {
		if (px_bridge_cfg_stale(s)) return ALP_ERR_NOT_READY;
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
		if (px_bridge_cfg_stale(s)) return ALP_ERR_NOT_READY;
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
