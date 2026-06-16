/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr QDEC backend.  Zephyr surfaces quadrature
 * decoders through the sensor_* API; SENSOR_CHAN_ROTATION returns
 * degrees in val1.  Used on any SoC unless gd32_bridge registers
 * a more specific match.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "qenc_ops.h"

#define ALP_QENC_DEV_OR_NULL(idx)                                                                  \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_qenc, idx))),                                  \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_qenc, idx)))),                                 \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_QENC_DEV_OR_NULL(0),
	ALP_QENC_DEV_OR_NULL(1),
	ALP_QENC_DEV_OR_NULL(2),
	ALP_QENC_DEV_OR_NULL(3),
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

static alp_status_t
z_open(const alp_qenc_config_t *cfg, alp_qenc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->encoder_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	const struct device *dev = _devs[cfg->encoder_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	st->dev           = (void *)dev;
	st->encoder_id    = cfg->encoder_id;
	st->last_position = 0;
	caps_out->flags   = 0u;
	return ALP_OK;
}

static alp_status_t z_get_position(alp_qenc_backend_state_t *st, int32_t *pos_out)
{
	const struct device *dev = (const struct device *)st->dev;
	int                  err = sensor_sample_fetch(dev);
	if (err != 0) return _errno_to_alp(err);
	struct sensor_value v;
	err = sensor_channel_get(dev, SENSOR_CHAN_ROTATION, &v);
	if (err != 0) return _errno_to_alp(err);
	/* Sensor value: degrees in val1 + microdegrees in val2; accumulate
     * integer degrees as a position proxy.  Real pulse counts come via
     * the v0.3 input-subsystem fast-path. */
	st->last_position += v.val1;
	*pos_out = st->last_position;
	return ALP_OK;
}

static alp_status_t z_reset_position(alp_qenc_backend_state_t *st)
{
	st->last_position = 0;
	return ALP_OK;
}

static const alp_qenc_ops_t _ops = {
	.open           = z_open,
	.get_position   = z_get_position,
	.reset_position = z_reset_position,
	.close          = NULL,
};

ALP_BACKEND_REGISTER(qenc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
