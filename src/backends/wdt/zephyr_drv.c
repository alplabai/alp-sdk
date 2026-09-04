/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr wdt_* driver-class backend.  Used on every SoC
 * the SDK ships unless a vendor-specific backend registers a more
 * specific match.
 */

#include <errno.h>

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

static alp_status_t
z_open(const alp_wdt_config_t *cfg, alp_wdt_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	const uint32_t wdt_id = cfg->wdt_id;
	if (wdt_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (wdt_id >= ALP_SOC_WDT_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[wdt_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	st->dev                     = (void *)dev;
	st->wdt_id                  = wdt_id;
	st->cfg                     = *cfg;
	struct wdt_timeout_cfg zcfg = {
		.window   = { .min = 0u, .max = cfg->timeout_ms },
		.callback = NULL,
		.flags    = (cfg->on_timeout == ALP_WDT_INTERRUPT_ONLY)
		                ? WDT_FLAG_RESET_NONE
		                : (cfg->on_timeout == ALP_WDT_RESET_CPU ? WDT_FLAG_RESET_CPU_CORE
		                                                        : WDT_FLAG_RESET_SOC),
	};
	int channel_id = wdt_install_timeout(dev, &zcfg);
	if (channel_id < 0) return _errno_to_alp(channel_id);
	st->channel_id = channel_id;
	int err        = wdt_setup(dev, 0);
	if (err != 0) return _errno_to_alp(err);
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
	const struct device *dev = (const struct device *)st->dev;
	/* Most M-class watchdogs don't allow disable; ignore error. */
	(void)wdt_disable(dev);
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
