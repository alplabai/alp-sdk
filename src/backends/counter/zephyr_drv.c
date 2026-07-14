/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr counter_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend (e.g. gd32_bridge) registers a
 * more specific silicon_ref match.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "counter_ops.h"

#define ALP_COUNTER_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_counter, idx))), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_counter, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_COUNTER_DEV_OR_NULL(0),
	ALP_COUNTER_DEV_OR_NULL(1),
	ALP_COUNTER_DEV_OR_NULL(2),
	ALP_COUNTER_DEV_OR_NULL(3),
};

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static void
_alarm_trampoline(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data)
{
	(void)dev;
	(void)chan_id;
	struct alp_counter *owner = (struct alp_counter *)user_data;
	if (owner == NULL || owner->state.alarm_cb == NULL) return;
	owner->state.alarm_cb(owner, ticks, owner->state.alarm_user);
}

static alp_status_t z_open(const alp_counter_config_t  *cfg,
                           alp_counter_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	if (cfg->counter_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	const struct device *dev = _devs[cfg->counter_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	st->dev         = (void *)dev;
	st->counter_id  = cfg->counter_id;
	caps_out->flags = 0u; /* Slice 4a: HW_ALARM cap flag deferred */
	return ALP_OK;
}

static alp_status_t z_start(alp_counter_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(counter_start(dev));
}

static alp_status_t z_stop(alp_counter_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(counter_stop(dev));
}

static alp_status_t z_get_value(alp_counter_backend_state_t *st, uint32_t *ticks_out)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(counter_get_value(dev, ticks_out));
}

static alp_status_t z_us_to_ticks(alp_counter_backend_state_t *st, uint32_t us, uint32_t *ticks_out)
{
	const struct device *dev = (const struct device *)st->dev;
	*ticks_out               = counter_us_to_ticks(dev, us);
	return ALP_OK;
}

static alp_status_t
z_set_alarm(alp_counter_backend_state_t *st, uint32_t ticks_from_now, struct alp_counter *owner)
{
	const struct device     *dev  = (const struct device *)st->dev;
	struct counter_alarm_cfg acfg = {
		.callback  = _alarm_trampoline,
		.ticks     = ticks_from_now,
		.user_data = owner, /* trampoline reaches alarm_cb via owner */
		.flags     = 0,
	};
	return _errno_to_alp(counter_set_channel_alarm(dev, 0, &acfg));
}

static alp_status_t z_cancel_alarm(alp_counter_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(counter_cancel_channel_alarm(dev, 0));
}

static void z_close(alp_counter_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	(void)counter_stop(dev);
}

static const alp_counter_ops_t _ops = {
	.open         = z_open,
	.start        = z_start,
	.stop         = z_stop,
	.get_value    = z_get_value,
	.us_to_ticks  = z_us_to_ticks,
	.set_alarm    = z_set_alarm,
	.cancel_alarm = z_cancel_alarm,
	.close        = z_close,
};

ALP_BACKEND_REGISTER(counter,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
