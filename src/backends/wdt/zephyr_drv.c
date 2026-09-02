/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr wdt_* driver-class backend.  Used on every SoC
 * the SDK ships unless a vendor-specific backend registers a more
 * specific match.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/wdt.h>

#include "alp_errno.h"
#include "wdt_ops.h"

#define ALP_WDT_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_wdt, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_wdt, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_WDT_DEV_OR_NULL(0),
	ALP_WDT_DEV_OR_NULL(1),
};

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * BEHAVIOUR CHANGE: this switch had no -EAGAIN and/or no -ETIMEDOUT
	 * arm, so a driver-reported deadline surfaced as ALP_ERR_IO.  Callers
	 * can now receive ALP_ERR_TIMEOUT here, and ALP_ERR_NOT_READY /
	 * ALP_ERR_NOMEM / ALP_ERR_NOSUPPORT for the other arms the switch
	 * lacked.  Every arm it DID carry agreed with the baseline. */
	return alp_status_from_zephyr_errno(err);
}

/* ISR-reachable expiry notification for ALP_WDT_INTERRUPT_ONLY.
 * Zephyr's wdt_callback_t is `void (*)(const struct device *, int
 * channel_id)` -- unlike counter's counter_alarm_cfg.user_data, it
 * carries no cookie of its own, so open() registers (dev, channel_id)
 * -> owner here and the trampoline below scans this small table
 * (bounded by ARRAY_SIZE(_devs), the same bound as wdt_id) (#1637).
 *
 * close() clears its entry before returning, but Zephyr's wdt API has
 * no per-channel uninstall -- a timeout already latched in hardware
 * can still fire after that, same as the counter backend's z_close()
 * comment on #1627 describes for its channel.  The owner==NULL check
 * in the trampoline turns that into a silent no-op instead of touching
 * freed/reused state; it cannot make the interrupt not fire.  Bounding
 * that window needs a bench measurement on real silicon -- needs-
 * silicon, not verifiable from native_sim. */
static struct {
	const struct device *dev;
	int                  channel_id;
	struct alp_wdt      *owner; /* NULL = unregistered; set/cleared with
                                    * release/acquire so the trampoline
                                    * either sees a fully-published owner
                                    * or none at all. */
} _expiry[ARRAY_SIZE(_devs)];

static void _expiry_trampoline(const struct device *dev, int channel_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(_expiry); ++i) {
		struct alp_wdt *owner =
		    (struct alp_wdt *)__atomic_load_n(&_expiry[i].owner, __ATOMIC_ACQUIRE);
		if (owner == NULL || _expiry[i].dev != dev || _expiry[i].channel_id != channel_id) {
			continue;
		}
		if (owner->state.cfg.on_expire != NULL) {
			owner->state.cfg.on_expire((alp_wdt_t *)owner, owner->state.cfg.user);
		}
		return;
	}
}

static alp_status_t
z_open(const alp_wdt_config_t *cfg, alp_wdt_backend_state_t *st, alp_capabilities_t *caps_out)
{
	/* The owner back-ref the ISR trampoline needs (Zephyr's
	 * wdt_callback_t carries no user_data cookie of its own, unlike
	 * counter_alarm_cfg's) is recovered with CONTAINER_OF instead of
	 * being threaded through the vtable: the dispatcher always calls
	 * ops->open(cfg, &h->state, ...) with st == &h->state and `state`
	 * is struct alp_wdt's first member (wdt_ops.h), and no wdt backend
	 * delegates the way the CC3501E GPIO proxy does (gpio_ops.h), so
	 * that recovery is always correct here -- no per-backend owner
	 * plumbing needed (#1637). */
	struct alp_wdt *owner  = CONTAINER_OF(st, struct alp_wdt, state);
	const uint32_t  wdt_id = cfg->wdt_id;
	if (wdt_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (wdt_id >= ALP_SOC_WDT_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[wdt_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	st->dev                               = (void *)dev;
	st->wdt_id                            = wdt_id;
	st->cfg                               = *cfg;
	const bool             interrupt_only = (cfg->on_timeout == ALP_WDT_INTERRUPT_ONLY);
	struct wdt_timeout_cfg zcfg           = {
		.window   = { .min = 0u, .max = cfg->timeout_ms },
		.callback = interrupt_only ? _expiry_trampoline : NULL,
		.flags    = interrupt_only ? WDT_FLAG_RESET_NONE
		                           : (cfg->on_timeout == ALP_WDT_RESET_CPU ? WDT_FLAG_RESET_CPU_CORE
		                                                                   : WDT_FLAG_RESET_SOC),
	};
	int channel_id = wdt_install_timeout(dev, &zcfg);
	if (channel_id == -EBUSY) {
		/* The dispatcher's per-wdt_id exclusivity (src/wdt_dispatch.c,
		 * #1650) already refused this call if another live alp_wdt_t
		 * handle owns wdt_id, so -EBUSY here can only mean the device
		 * itself is still wdt_setup() from an EARLIER handle that
		 * closed without an explicit alp_wdt_disable() first --
		 * wdt_install_timeout() "must be used before wdt_setup()" and
		 * refuses a second install otherwise (no per-channel
		 * uninstall to undo just the old one).  Reclaim the device
		 * with one wdt_disable() + retry rather than leaving this
		 * wdt_id un-reopenable.  Safe for the SDK's own handles by
		 * construction (exclusivity already rules out a live
		 * sibling); a non-SDK Zephyr consumer sharing this exact
		 * device (e.g. CONFIG_TASK_WDT) is the one case this can
		 * still disarm -- the same residual exposure the old
		 * unconditional close()-time wdt_disable() carried, just now
		 * triggered only on an actual reopen instead of on every
		 * close (#1637).
		 *
		 * wdt_disable()'s own status is checked, not discarded: not
		 * every watchdog CAN be disabled once armed (Zephyr's
		 * watchdog.h: "not all watchdogs can be restarted after they
		 * are disabled").  A driver that refuses (e.g. -EPERM) leaves
		 * the device exactly as armed as before, so retrying install
		 * would just re-fail -EBUSY against a device we already know
		 * is still armed -- return disable_rc itself instead, which
		 * names the real reason reclaim didn't happen rather than
		 * repeating the stale first -EBUSY (that stale repeat is the
		 * case wdt.h's ALP_ERR_BUSY doc below still describes: a
		 * live non-SDK consumer, not a disable refusal).
		 *
		 * When disable_rc IS 0, the retry below can still fail
		 * (wdt_install_timeout() or the wdt_setup() call further
		 * down) -- Zephyr's WDT API has no way to undo a successful
		 * disable or restore the timeout it replaced, so a caller
		 * that had relied on this wdt_id's post-close armed state
		 * (wdt.h's alp_wdt_close() doc) loses that protection on this
		 * failure path with no dedicated status of its own.  Callers
		 * for whom that residual gap matters should not depend on the
		 * post-close armed state surviving a later open() attempt on
		 * the same wdt_id. */
		int disable_rc = wdt_disable(dev);
		if (disable_rc != 0) {
			return _errno_to_alp(disable_rc);
		}
		channel_id = wdt_install_timeout(dev, &zcfg);
	}
	if (channel_id < 0) return _errno_to_alp(channel_id);
	st->channel_id = channel_id;
	int err        = wdt_setup(dev, 0);
	if (err != 0) return _errno_to_alp(err);
	if (interrupt_only) {
		/* Publish AFTER wdt_setup succeeds: on any earlier return the
		 * dispatcher frees this slot without calling close(), so a
		 * registration written before a failure path would leak a
		 * dangling owner. */
		_expiry[wdt_id].dev        = dev;
		_expiry[wdt_id].channel_id = channel_id;
		__atomic_store_n(&_expiry[wdt_id].owner, owner, __ATOMIC_RELEASE);
	}
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_feed(alp_wdt_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(wdt_feed(dev, st->channel_id));
}

static alp_status_t z_disable(alp_wdt_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(wdt_disable(dev));
}

static void z_close(alp_wdt_backend_state_t *st)
{
	/* Does NOT call wdt_disable(dev): that disarms the WHOLE device,
	 * not just this handle's channel -- Zephyr's wdt_* driver class has
	 * no per-channel disable.  The dispatcher's per-wdt_id exclusivity
	 * (src/wdt_dispatch.c, #1650) already rules out a second alp_wdt_t
	 * handle on this same device, so the reachable risk this guards
	 * against is a non-SDK Zephyr consumer sharing the device (e.g.
	 * CONFIG_TASK_WDT installing its own channel) -- disabling here
	 * would silently pull that consumer's protection out from under
	 * it, which was never this handle's decision to make for the
	 * device as a whole (#1637).  A caller that wants a best-effort
	 * SoC-wide disable calls alp_wdt_disable() explicitly before
	 * close() -- see its ALP_ERR_NOSUPPORT contract.  Leaving the
	 * device armed here does not strand this wdt_id on hardware that
	 * can actually be disabled: z_open()'s own -EBUSY handling above
	 * reclaims it on the next open.  On a watchdog that refuses
	 * wdt_disable() once armed (Zephyr watchdog.h: "not all watchdogs
	 * can be restarted after they are disabled"), that reclaim itself
	 * fails and surfaces the driver's own status instead of retrying
	 * -- see z_open()'s comment -- and this wdt_id genuinely stays
	 * un-reopenable until reset, a hardware limit, not something this
	 * SDK can work around without a Zephyr WDT API this class of
	 * device doesn't offer.
	 *
	 * Unregister the ISR trampoline so a timeout that fires after this
	 * point cannot resolve to this (about-to-be-freed) owner -- see the
	 * _expiry[] comment above for why this narrows, but cannot close,
	 * that window without silicon. */
	if (st->wdt_id < ARRAY_SIZE(_expiry)) {
		__atomic_store_n(&_expiry[st->wdt_id].owner, NULL, __ATOMIC_RELEASE);
	}
}

static const alp_wdt_ops_t _ops = {
	.open    = z_open,
	.feed    = z_feed,
	.disable = z_disable,
	.close   = z_close,
};

ALP_BACKEND_REGISTER(wdt,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
