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
 *
 * The exact wall-clock latency + retained-state guarantees depend
 * on the active SoC's pm_state table (DT-defined `cpu-power-states`
 * on the cpu node).  Backends pin a richer per-SoC mapping at a
 * silicon_ref-specific priority above this wildcard if they need
 * different semantics; this file is the family-portable default.
 *
 * Wake-source handling
 * --------------------
 * The portable bitmap @c ALP_POWER_WAKE_* is informational at this
 * layer: Zephyr's pm_policy doesn't take a wake-source mask.  The
 * actual wake-source enablement happens at the per-vendor HAL
 * level (RTC alarm via the configured alp_rtc_*; GPIO IRQ via
 * alp_gpio_*; UART RX via the LPUART driver).  When the caller
 * passes @p wake_after_ms > 0 we register a Zephyr k_timer to
 * guarantee the wake, which covers the most common case (RTC
 * timed wake).  GPIO / UART / USB wakes rely on the caller having
 * configured the matching IRQ source before calling sleep -- the
 * setup of those bits is the caller's responsibility.
 *
 * Yocto / Linux deferral
 * ----------------------
 * The /sys/power/state-write Yocto path is intentionally not
 * implemented here -- per the standing "src/yocto/ off-limits"
 * guardrail it lands in a dedicated slice when scheduled.  Customers
 * on a Linux backend should expect alp_power_request_sleep to
 * surface NOSUPPORT until that lands.  See the matching comment in
 * src/backends/power/zephyr_stub.c which carries the same note.
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
		return PM_STATE_SUSPEND_TO_RAM;
	/* ALP_POWER_MODE_RUN is filtered by the dispatcher; only the
     * three sleep modes reach the backend. */
	default:
		return PM_STATE_SUSPEND_TO_IDLE;
	}
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
		if (!_lock_in_use[i]) {
			_lock_pool[i]   = (pm_locks_t){ 0 };
			_lock_in_use[i] = true;
			return &_lock_pool[i];
		}
	}
	return NULL;
}

static void _free_locks(pm_locks_t *l)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_POWER_HANDLES; ++i) {
		if (&_lock_pool[i] == l) {
			_lock_in_use[i] = false;
			return;
		}
	}
}

/* Sleep-completion semaphore.  k_timer expiry signals it; the
 * idle thread also signals on an OOB wake (e.g. GPIO IRQ that
 * runs a wake handler).  Single-instance is fine because the
 * dispatcher serialises sleep requests through its one-slot
 * handle pool by default. */
static K_SEM_DEFINE(_wake_sem, 0, 1);

static void _timer_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&_wake_sem);
}

static K_TIMER_DEFINE(_wake_timer, _timer_expiry, NULL);

static alp_status_t z_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out)
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
	return ALP_OK;
}

static alp_status_t z_configure_wake_source(alp_power_backend_state_t *state, uint32_t wake_bitmap)
{
	/* Bitmap is mirrored by the dispatcher in state->wake_bitmap;
     * the per-source IRQ enablement is owned by the matching
     * peripheral driver (alp_rtc_*, alp_gpio_*).  This backend
     * only consumes the bitmap to decide whether to register a
     * k_timer-only wake at request_sleep() time. */
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

	/* Allow the policy to descend at most as far as `mode`. */
	_adjust_locks_for_request(l, mode);

	/* Set up the timer wake if requested.  zero-len timer is
     * legal here -- a non-timer wake source must already have
     * been armed by the caller's pre-sleep setup. */
	k_sem_reset(&_wake_sem);
	if (wake_after_ms > 0u) {
		k_timer_start(&_wake_timer, K_MSEC(wake_after_ms), K_NO_WAIT);
	}

	/* Park until the timer expires (or an OOB wake handler signals
     * us).  The actual descent into the requested pm_state happens
     * inside the idle thread while this take blocks. */
	int64_t before = k_uptime_get();
	int     err =
	    k_sem_take(&_wake_sem, (wake_after_ms > 0u) ? K_MSEC(wake_after_ms + 100u) : K_FOREVER);
	int64_t after = k_uptime_get();
	if (wake_after_ms > 0u) {
		k_timer_stop(&_wake_timer);
	}

	/* Re-hold every lock so the system stays ACTIVE until the next
     * explicit request_sleep. */
	_hold_all_locks(l);

	if (info != NULL) {
		info->realised_mode = mode;
		/* Wake-source attribution is best-effort: if the caller
         * passed wake_after_ms > 0 and we returned because of the
         * timer, report RTC; otherwise zero (unknown).  Richer
         * attribution requires per-source wake handlers writing a
         * tagged value into shared state -- deferred. */
		info->wake_source = (wake_after_ms > 0u && err == 0) ? (uint32_t)ALP_POWER_WAKE_RTC : 0u;
		info->slept_ms    = (uint32_t)((after > before) ? (after - before) : 0);
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
