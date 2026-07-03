/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr i2c_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/i2c_dispatch.c; the
 * backend's open fills state->dev and configures the bus speed.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "i2c_ops.h"

#define ALP_I2C_DEV_OR_NULL(idx)                                                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_i2c, idx))),                                   \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2c, idx)))),                                  \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_I2C_DEV_OR_NULL(0), ALP_I2C_DEV_OR_NULL(1), ALP_I2C_DEV_OR_NULL(2), ALP_I2C_DEV_OR_NULL(3),
	ALP_I2C_DEV_OR_NULL(4), ALP_I2C_DEV_OR_NULL(5), ALP_I2C_DEV_OR_NULL(6), ALP_I2C_DEV_OR_NULL(7),
};

static uint32_t _alp_to_zephyr_bitrate_flags(uint32_t bitrate_hz)
{
	if (bitrate_hz >= 1000000) return I2C_SPEED_SET(I2C_SPEED_FAST_PLUS);
	if (bitrate_hz >= 400000) return I2C_SPEED_SET(I2C_SPEED_FAST);
	return I2C_SPEED_SET(I2C_SPEED_STANDARD);
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t
z_open(const alp_i2c_config_t *cfg, alp_i2c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->bus_id >= ALP_SOC_I2C_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	uint32_t flags = I2C_MODE_CONTROLLER | _alp_to_zephyr_bitrate_flags(cfg->bitrate_hz);
	int      err   = i2c_configure(dev, flags);
	if (err != 0) return _errno_to_alp(err);
	st->dev         = (void *)dev;
	st->bus_id      = cfg->bus_id;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
z_write(alp_i2c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(i2c_write(dev, data, len, addr));
}

static alp_status_t z_read(alp_i2c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(i2c_read(dev, data, len, addr));
}

static alp_status_t z_write_read(alp_i2c_backend_state_t *st,
                                 uint8_t                  addr,
                                 const uint8_t           *wdata,
                                 size_t                   wlen,
                                 uint8_t                 *rdata,
                                 size_t                   rlen)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(i2c_write_read(dev, addr, wdata, wlen, rdata, rlen));
}

/* ------------------------------------------------------------------ */
/* Target (slave) mode -- over Zephyr's i2c_target_register API.       */
/*                                                                     */
/* Per-handle sidecar: Zephyr keeps a pointer to the registered        */
/* struct i2c_target_config for the lifetime of the registration, so   */
/* it must live in static storage, not on the open() caller's stack.   */
/* The alp-level callbacks are recovered via CONTAINER_OF from the     */
/* i2c_target_config the driver hands back to each ISR callback.       */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES 2
#endif

typedef struct {
	struct i2c_target_config zcfg; /* registered with the driver; must outlive it */
	alp_i2c_target_config_t  app;  /* caller's callbacks + user pointer */
	bool                     in_use;
} alp_z_i2c_target_side_t;

static alp_z_i2c_target_side_t _tsides[CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES];

static alp_z_i2c_target_side_t *_talloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_tsides); ++i) {
		/* Atomic claim (see alp_slot_claim.h): in_use is the last
		 * member, so the winner zeroes everything before it. */
		if (alp_slot_try_claim(&_tsides[i].in_use)) {
			memset(&_tsides[i], 0, offsetof(alp_z_i2c_target_side_t, in_use));
			return &_tsides[i];
		}
	}
	return NULL;
}

static int zt_write_requested(struct i2c_target_config *config)
{
	ARG_UNUSED(config);
	return 0; /* accept the write phase; bytes arrive via write_received */
}

static int zt_write_received(struct i2c_target_config *config, uint8_t val)
{
	alp_z_i2c_target_side_t *s = CONTAINER_OF(config, alp_z_i2c_target_side_t, zcfg);
	s->app.on_write(val, s->app.user);
	return 0;
}

static int zt_read_requested(struct i2c_target_config *config, uint8_t *val)
{
	alp_z_i2c_target_side_t *s = CONTAINER_OF(config, alp_z_i2c_target_side_t, zcfg);
	return (s->app.on_read(val, s->app.user) == ALP_OK) ? 0 : -EIO;
}

static int zt_read_processed(struct i2c_target_config *config, uint8_t *val)
{
	/* Same contract as read_requested: supply the next byte. */
	return zt_read_requested(config, val);
}

static int zt_stop(struct i2c_target_config *config)
{
	alp_z_i2c_target_side_t *s = CONTAINER_OF(config, alp_z_i2c_target_side_t, zcfg);
	if (s->app.on_stop != NULL) {
		s->app.on_stop(s->app.user);
	}
	return 0;
}

static const struct i2c_target_callbacks _tcallbacks = {
	.write_requested = zt_write_requested,
	.write_received  = zt_write_received,
	.read_requested  = zt_read_requested,
	.read_processed  = zt_read_processed,
	.stop            = zt_stop,
};

static alp_status_t z_target_open(const alp_i2c_target_config_t *cfg, alp_i2c_backend_state_t *st)
{
	if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->bus_id >= ALP_SOC_I2C_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

	alp_z_i2c_target_side_t *s = _talloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	s->app            = *cfg;
	s->zcfg.address   = cfg->own_addr_7bit;
	s->zcfg.flags     = 0u; /* 7-bit addressing */
	s->zcfg.callbacks = &_tcallbacks;

	/* Drivers without target support return -ENOSYS here (mapped to
	 * ALP_ERR_NOSUPPORT) -- the native_sim emul controller path. */
	int err = i2c_target_register(dev, &s->zcfg);
	if (err != 0) {
		alp_slot_release(&s->in_use);
		return _errno_to_alp(err);
	}
	st->dev     = (void *)dev;
	st->bus_id  = cfg->bus_id;
	st->be_data = s;
	return ALP_OK;
}

static void z_target_close(alp_i2c_backend_state_t *st)
{
	alp_z_i2c_target_side_t *s   = (alp_z_i2c_target_side_t *)st->be_data;
	const struct device     *dev = (const struct device *)st->dev;
	if (s == NULL) return;
	(void)i2c_target_unregister(dev, &s->zcfg);
	alp_slot_release(&s->in_use);
	st->be_data = NULL;
}

static const alp_i2c_ops_t _ops = {
	.open         = z_open,
	.write        = z_write,
	.read         = z_read,
	.write_read   = z_write_read,
	.close        = NULL, /* no teardown needed for i2c_configure */
	.target_open  = z_target_open,
	.target_close = z_target_close,
};

ALP_BACKEND_REGISTER(i2c,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
