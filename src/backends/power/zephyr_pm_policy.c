/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Zephyr power backend using `pm_policy_state_lock_get/put`
 * from <zephyr/pm/policy.h>.  Registered as silicon_ref="*" at
 * priority 100 so it always wins over the zephyr_stub fallback
 * (priority 0) on builds that link Zephyr's PM subsystem.
 *
 * Translation model
 * -----------------
 * Zephyr's pm_policy_* API is *constraint-shaped*, not transition-
 * shaped: callers `_lock_get(state)` to disallow a state and
 * `_lock_put(state)` to release it.  When idle and no locks are
 * held, the system falls into the deepest pm_state the policy
 * picks based on the next scheduled event (RTC alarm, timer,
 * etc.).  Our `alp_power_request_sleep` contract therefore maps:
 *
 *   1. Release the lock for every state at-or-deeper than the
 *      requested mode (so the policy is free to descend that far).
 *   2. Hold a lock on every state *deeper* than the requested
 *      mode (so the policy can't go further than the caller asked
 *      for -- e.g. STANDBY -> SUSPEND_TO_RAM only, never
 *      SUSPEND_TO_DISK / SOFT_OFF).
 *   3. Set a Zephyr k_timer (or rely on the configured wake
 *      source) so the idle thread has a known next-event to
 *      compute against.
 *   4. Park on a semaphore until the wake source fires.
 *   5. On wake: re-acquire the locks the caller would have held
 *      pre-sleep so the system stays in the "running" state until
 *      the next explicit request_sleep call.
 *
 * The mapping ALP_POWER_MODE_* -> enum pm_state is:
 *
 *   ALP_POWER_MODE_SLEEP       -> PM_STATE_SUSPEND_TO_IDLE
 *   ALP_POWER_MODE_DEEP_SLEEP  -> PM_STATE_STANDBY
 *   ALP_POWER_MODE_STANDBY     -> PM_STATE_SUSPEND_TO_RAM
 *   ALP_POWER_MODE_STOP        -> PM_STATE_SUSPEND_TO_RAM (round-down:
 *                                 this generic backend has nothing
 *                                 deeper; realised_mode reports
 *                                 STANDBY, not STOP -- see
 *                                 _realised_mode, #1813)
 *
 * The exact wall-clock latency + retained-state guarantees depend
 * on the active SoC's pm_state table (DT-defined `cpu-power-states`
 * on the cpu node).  Backends pin a richer per-SoC mapping at a
 * silicon_ref-specific priority above this wildcard if they need
 * different semantics; this file is the family-portable default.
 *
 * Wake-source handling
 * --------------------
 * This layer's ONLY actual wake mechanism is its own Zephyr k_timer
 * (_wake_timer / _timer_expiry below): _wake_sem has exactly one
 * giver in this file, armed only when @p wake_after_ms > 0.  There is
 * NO out-of-band signaller wired to RTC alarms, GPIO IRQs, or UART RX
 * -- a caller can configure @c alp_rtc_* / @c alp_gpio_* however it
 * likes, but nothing here ever learns about it or gives @c _wake_sem
 * on its account, so z_request_sleep would park on K_FOREVER forever.
 * An earlier draft advertised @c ALP_POWER_WAKE_RTC / @c _GPIO /
 * @c _UART_RX here on the theory that the caller's own setup would
 * cover them; review caught that this backend never actually wakes
 * on them -- the exact #1812 shape, reincarnated on a second backend.
 * z_open() therefore reports ONLY @c ALP_POWER_WAKE_TIMER via
 * wake_caps_out (the dispatcher enforces that against every
 * alp_power_configure_wake_source() call, #1813), and z_request_sleep
 * refuses @p wake_after_ms == 0 outright with @ref ALP_ERR_NOSUPPORT
 * rather than trust the bitmap alone -- belt and suspenders, since a
 * caller can still configure @c ALP_POWER_WAKE_TIMER and pass
 * wake_after_ms == 0 by mistake (the dispatcher's own INVAL guard
 * only catches bitmap == 0 && wake_after_ms == 0, not a non-empty
 * bitmap paired with wake_after_ms == 0).
 *
 * Yocto / Linux path
 * ------------------
 * This file is Zephyr-only (it includes <zephyr/pm/policy.h>) and is
 * never compiled into a Yocto build.  The /sys/power/state +
 * /sys/class/rtc/rtc0/wakealarm path lives in the sibling
 * src/backends/power/yocto_drv.c (#613), registered at the same
 * priority 100 for silicon_ref "*" on Linux.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/state.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "alp_slot_claim.h"
#include "power_ops.h"

/* Held-lock bookkeeping so close() / repeated request_sleep can
 * unwind without leaking pm_policy lock references.  The Power
 * dispatcher caps live handles at 1 by default (one PMU, one
 * handle), so a single static is enough; if
 * CONFIG_ALP_SDK_MAX_POWER_HANDLES is overridden the dispatcher
 * still serialises calls onto the policy, so per-handle state is
 * still correct in aggregate. */
typedef struct {
	bool sleep_lock_held;      /* PM_STATE_SUSPEND_TO_IDLE held? */
	bool deep_sleep_lock_held; /* PM_STATE_STANDBY held?         */
	bool standby_lock_held;    /* PM_STATE_SUSPEND_TO_RAM held?  */
} pm_locks_t;

static enum pm_state _to_pm_state(alp_power_mode_t mode)
{
	switch (mode) {
	case ALP_POWER_MODE_SLEEP:
		return PM_STATE_SUSPEND_TO_IDLE;
	case ALP_POWER_MODE_DEEP_SLEEP:
		return PM_STATE_STANDBY;
	case ALP_POWER_MODE_STANDBY:
	case ALP_POWER_MODE_STOP:
		/* This generic "*" backend has nothing deeper than
         * PM_STATE_SUSPEND_TO_RAM to offer -- STOP rounds DOWN to it
         * (the monotonic-mode contract <alp/power.h> documents).
         * Explicit case, not the default below: falling through to
         * default would round STOP to the SHALLOWEST state instead
         * of the deepest, silently entering SUSPEND_TO_IDLE while
         * telling the caller STOP was realised (#1813 review).
         * z_request_sleep separately reports realised_mode as
         * STANDBY, not STOP, so the caller is never told this
         * backend reached a depth it never proved -- see
         * _realised_mode below. */
		return PM_STATE_SUSPEND_TO_RAM;
	/* ALP_POWER_MODE_RUN is filtered by the dispatcher; only the
     * sleep modes above reach the backend. */
	default:
		return PM_STATE_SUSPEND_TO_IDLE;
	}
}

/* What alp_power_wake_info_t::realised_mode reports for a given
 * REQUESTED mode, given the round-down _to_pm_state performs above.
 * Every mode maps to itself except STOP, which this backend can only
 * realise as STANDBY (both target PM_STATE_SUSPEND_TO_RAM) -- so the
 * caller learns the true depth reached, never the deeper one asked
 * for (#1813 review: "a two-orders-of-magnitude power lie"). */
static alp_power_mode_t _realised_mode(alp_power_mode_t requested)
{
	return (requested == ALP_POWER_MODE_STOP) ? ALP_POWER_MODE_STANDBY : requested;
}

/* On entry: caller wants the system to be free to descend into
 * @p mode.  Release any lock currently held at that depth; hold
 * locks on every state *deeper* than @p mode so the policy can't
 * over-descend. */
static void _adjust_locks_for_request(pm_locks_t *l, alp_power_mode_t mode)
{
	/* Release the lock matching this depth (so policy can pick it). */
	enum pm_state want = _to_pm_state(mode);

	if (want == PM_STATE_SUSPEND_TO_IDLE && l->sleep_lock_held) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		l->sleep_lock_held = false;
	}
	if (want == PM_STATE_STANDBY && l->deep_sleep_lock_held) {
		pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		l->deep_sleep_lock_held = false;
	}
	if (want == PM_STATE_SUSPEND_TO_RAM && l->standby_lock_held) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
		l->standby_lock_held = false;
	}

	/* Disallow the *deeper* states so the policy never goes past
     * what the caller asked for.  Lock-get is reference-counted, so
     * re-acquiring a lock we already hold is a no-op increment. */
	if (mode == ALP_POWER_MODE_SLEEP) {
		if (!l->deep_sleep_lock_held) {
			pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
			l->deep_sleep_lock_held = true;
		}
		if (!l->standby_lock_held) {
			pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
			l->standby_lock_held = true;
		}
	} else if (mode == ALP_POWER_MODE_DEEP_SLEEP) {
		if (!l->standby_lock_held) {
			pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
			l->standby_lock_held = true;
		}
	}
	/* STANDBY: nothing deeper we should block (SOFT_OFF is power-off,
     * not a sleep state; the caller can reach it through a separate
     * dedicated API once that lands). */
}

/* On exit: re-hold every lock so the policy stays in ACTIVE until
 * the next explicit request_sleep call.  Idempotent with the
 * release path: only acquires what is currently un-held. */
static void _hold_all_locks(pm_locks_t *l)
{
	if (!l->sleep_lock_held) {
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		l->sleep_lock_held = true;
	}
	if (!l->deep_sleep_lock_held) {
		pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		l->deep_sleep_lock_held = true;
	}
	if (!l->standby_lock_held) {
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
		l->standby_lock_held = true;
	}
}

static void _release_all_locks(pm_locks_t *l)
{
	if (l->sleep_lock_held) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		l->sleep_lock_held = false;
	}
	if (l->deep_sleep_lock_held) {
		pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		l->deep_sleep_lock_held = false;
	}
	if (l->standby_lock_held) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
		l->standby_lock_held = false;
	}
}

#ifndef CONFIG_ALP_SDK_MAX_POWER_HANDLES
#define CONFIG_ALP_SDK_MAX_POWER_HANDLES 1
#endif

static pm_locks_t _lock_pool[CONFIG_ALP_SDK_MAX_POWER_HANDLES];
static bool       _lock_in_use[CONFIG_ALP_SDK_MAX_POWER_HANDLES];

static pm_locks_t *_alloc_locks(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_POWER_HANDLES; ++i) {
		/* Atomic claim (src/common/alp_slot_claim.h, issue #1115):
		 * a compare-exchange, so exactly one concurrent opener wins the
		 * slot.  in_use lives in a parallel array rather than inside the
		 * slot struct, so the winner may zero the whole slot afterwards --
		 * no offsetof form is needed here. */
		if (alp_slot_try_claim(&_lock_in_use[i])) {
			_lock_pool[i] = (pm_locks_t){ 0 };
			return &_lock_pool[i];
		}
	}
	return NULL;
}

static void _free_locks(pm_locks_t *l)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_POWER_HANDLES; ++i) {
		if (&_lock_pool[i] == l) {
			alp_slot_release(&_lock_in_use[i]);
			return;
		}
	}
}

/* Sleep-completion semaphore.  _timer_expiry (k_timer expiry) is its
 * ONLY giver in this file -- there is no OOB signaller for a GPIO /
 * UART_RX / RTC-alarm wake, so a request with wake_after_ms == 0 has
 * nothing that will ever give this (see z_request_sleep's guard).
 * Single-instance is fine because the dispatcher serialises sleep
 * requests through its one-slot handle pool by default. */
static K_SEM_DEFINE(_wake_sem, 0, 1);

static void _timer_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&_wake_sem);
}

static K_TIMER_DEFINE(_wake_timer, _timer_expiry, NULL);

static alp_status_t
z_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out, uint32_t *wake_caps_out)
{
	pm_locks_t *l = _alloc_locks();
	if (l == NULL) {
		return ALP_ERR_NOMEM;
	}
	state->be_data = l;
	if (caps_out != NULL) {
		/* No portable per-handle caps to advertise at v0.5;
         * leaves the base_caps from the registry entry intact. */
		(void)caps_out;
	}
	/* This layer PARKS on _wake_sem until wake, and _timer_expiry (the
     * k_timer this file owns) is its ONLY giver -- report ONLY
     * ALP_POWER_WAKE_TIMER (real: wake_after_ms > 0 arms it and it
     * always fires).  RTC / GPIO / UART_RX / USB / ETH_LINK /
     * COMPARATOR / BROWNOUT are all claimed nowhere in this file: a
     * caller configuring alp_rtc_* / alp_gpio_* itself does not wire
     * that peripheral's IRQ to _wake_sem, so advertising those bits
     * would be exactly the #1812 lie on a second backend (caught in
     * review -- see the file header). */
	if (wake_caps_out != NULL) {
		*wake_caps_out = ALP_POWER_WAKE_TIMER;
	}
	return ALP_OK;
}

static alp_status_t z_configure_wake_source(alp_power_backend_state_t *state, uint32_t wake_bitmap)
{
	/* Only ALP_POWER_WAKE_TIMER can reach here -- the dispatcher
     * already rejected anything else against the wake_caps z_open
     * reported above (#1813).  Nothing to configure: the k_timer is
     * armed unconditionally by z_request_sleep whenever
     * wake_after_ms > 0, independent of this bitmap. */
	(void)state;
	(void)wake_bitmap;
	return ALP_OK;
}

static alp_status_t z_request_sleep(alp_power_backend_state_t *state,
                                    alp_power_mode_t           mode,
                                    uint32_t                   wake_after_ms,
                                    alp_power_wake_info_t     *info)
{
	pm_locks_t *l = (pm_locks_t *)state->be_data;
	if (l == NULL) {
		return ALP_ERR_NOT_READY;
	}

	/* _wake_sem's ONLY giver is _timer_expiry, armed below only when
     * wake_after_ms > 0 -- refuse outright rather than park on
     * K_FOREVER with nothing that will ever wake us.  Belt-and-
     * suspenders against z_open only advertising ALP_POWER_WAKE_TIMER:
     * the dispatcher's own INVAL guard catches bitmap == 0 &&
     * wake_after_ms == 0, but not a caller who configured
     * ALP_POWER_WAKE_TIMER and then passed wake_after_ms == 0 anyway. */
	if (wake_after_ms == 0u) {
		if (info != NULL) {
			info->realised_mode = ALP_POWER_MODE_RUN;
			info->wake_source   = 0u;
			info->slept_ms      = 0u;
		}
		return ALP_ERR_NOSUPPORT;
	}

	/* Allow the policy to descend at most as far as `mode`. */
	_adjust_locks_for_request(l, mode);

	/* Arm the timer wake -- the only wake source this backend has;
     * see the file header + z_open. */
	k_sem_reset(&_wake_sem);
	k_timer_start(&_wake_timer, K_MSEC(wake_after_ms), K_NO_WAIT);

	/* Park until the timer expires.  The actual descent into the
     * requested pm_state happens inside the idle thread while this
     * take blocks. */
	int64_t before = k_uptime_get();
	int     err    = k_sem_take(&_wake_sem, K_MSEC(wake_after_ms + 100u));
	int64_t after  = k_uptime_get();
	k_timer_stop(&_wake_timer);

	/* Re-hold every lock so the system stays ACTIVE until the next
     * explicit request_sleep. */
	_hold_all_locks(l);

	if (info != NULL) {
		info->realised_mode = _realised_mode(mode);
		info->wake_source   = (err == 0) ? (uint32_t)ALP_POWER_WAKE_TIMER : 0u;
		info->slept_ms      = (uint32_t)((after > before) ? (after - before) : 0);
	}

	if (err != 0 && err != -EAGAIN) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

static void z_close(alp_power_backend_state_t *state)
{
	if (state == NULL || state->be_data == NULL) {
		return;
	}
	pm_locks_t *l = (pm_locks_t *)state->be_data;
	_release_all_locks(l);
	_free_locks(l);
	state->be_data = NULL;
}

static const alp_power_ops_t _ops = {
	.open                  = z_open,
	.configure_wake_source = z_configure_wake_source,
	.request_sleep         = z_request_sleep,
	.close                 = z_close,
};

ALP_BACKEND_REGISTER(power,
                     zephyr_pm_policy,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
