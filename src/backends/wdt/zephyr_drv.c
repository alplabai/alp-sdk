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

static alp_status_t z_open(const alp_wdt_config_t  *cfg,
                           alp_wdt_backend_state_t *st,
                           alp_capabilities_t      *caps_out,
                           struct alp_wdt          *owner)
{
	const uint32_t wdt_id = cfg->wdt_id;
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
	 * no per-channel disable.  A second handle on a different channel
	 * of the same device would silently lose its protection the moment
	 * this handle closed (#1637).  The dispatcher's per-wdt_id
	 * exclusivity (src/wdt_dispatch.c, #1650) is what actually prevents
	 * two owners of ONE channel; disabling here was never this handle's
	 * decision to make for the device as a whole.  A caller that wants
	 * a best-effort SoC-wide disable calls alp_wdt_disable() explicitly
	 * before close() -- see its ALP_ERR_NOSUPPORT contract.
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
