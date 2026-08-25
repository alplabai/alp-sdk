/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr i3c_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.
 *
 * Each form-factor bus_id maps to the `alp-i3cN` devicetree alias.  Timing
 * (SCL rate, DAA) is devicetree-owned: open() only checks device_is_ready --
 * it never calls i3c_configure -- the legal rate on a mixed I3C/I2C bus
 * depends on the slowest device populated, which is a board fact, not a
 * per-open() choice.
 *
 * addr is the target's dynamic (or legacy static) address; write/read/
 * write_read resolve it to a `struct i3c_device_desc *` via
 * i3c_dev_list_i3c_addr_find() on the controller's attached-device list,
 * then issue i3c_transfer().  An address with no attached device is
 * ALP_ERR_IO (mirrors an I2C NACK -- "nothing answered").
 *
 * The Zephyr I3C subsystem isn't universally present, so the DT-alias body
 * only compiles when CONFIG_I3C_CONTROLLER=y -- NOT merely CONFIG_I3C=y.
 * i3c_transfer() and the i3c_dev_list_*_find() helpers this backend calls are
 * declared inside `#if defined(CONFIG_I3C_CONTROLLER)` in
 * zephyr/drivers/i3c.h, and i3c_common.c is built
 * `zephyr_library_sources_ifdef(CONFIG_I3C_CONTROLLER)`.  A target-role-only
 * build (CONFIG_I3C=y + CONFIG_I3C_TARGET_ROLE_ONLY=y, a legal choice under
 * `choice I3C_MODE`) has the subsystem but none of those symbols, so guarding
 * on CONFIG_I3C would fail to compile there.  When the controller role is
 * absent the backend still registers (keeping the class linker section
 * non-empty) but every op returns NOSUPPORT.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>

#include "alp_errno.h"
#include "i3c_ops.h"

#if defined(CONFIG_I3C_CONTROLLER)
#include <zephyr/drivers/i3c.h>

#define ALP_I3C_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_i3c, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i3c, idx)))), \
	            (NULL))

/* Two slots, mirroring alp_dac_devs[] in the DAC backend: the E8 has TWO I3C
 * controllers (i3c0 + lpi3c0, ALP_SOC_I3C_COUNT 2), and a slot whose
 * `alp-i3cN` alias is absent resolves to NULL rather than shrinking the table.
 * Sizing this to only the alias the carrier happens to declare would make
 * alp_i3c_open(bus_id = 1) report ALP_ERR_INVAL ("no such bus") on a part that
 * soc_caps says has two -- ALP_ERR_NOT_READY ("exists, not wired up") is the
 * honest answer, and that is what the NULL slot produces below. */
static const struct device *const alp_i3c_devs[] = {
	ALP_I3C_DEV_OR_NULL(0),
	ALP_I3C_DEV_OR_NULL(1),
};

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * This switch was one of 27 hand-copied copies that had drifted; the
	 * arms it carried all agreed with the baseline, so the mapping it
	 * produced for them is unchanged. */
	return alp_status_from_zephyr_errno(err);
}

static alp_status_t
z_open(const alp_i3c_config_t *cfg, alp_i3c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->bus_id >= ARRAY_SIZE(alp_i3c_devs)) return ALP_ERR_INVAL;
	const struct device *dev = alp_i3c_devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	/* Timing + DAA are devicetree-owned (i3c-scl-hz, od-thigh-min-ns);
	 * no i3c_configure() call here on purpose -- see file header. */
	st->dev         = (void *)dev;
	st->bus_id      = cfg->bus_id;
	caps_out->flags = 0u;
	return ALP_OK;
}

/* Resolve a target's dynamic (or legacy static) address to the attached
 * device descriptor the driver needs for i3c_transfer().  NULL if nothing
 * on the bus has been assigned this address (unknown/absent target). */
static struct i3c_device_desc *_resolve(const struct device *dev, uint8_t addr)
{
	struct i3c_device_desc *target = i3c_dev_list_i3c_addr_find(dev, addr);
	if (target == NULL) {
		target = i3c_dev_list_i3c_static_addr_find(dev, addr);
	}
	return target;
}

static alp_status_t
z_write(alp_i3c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	const struct device    *dev    = (const struct device *)st->dev;
	struct i3c_device_desc *target = _resolve(dev, addr);
	if (target == NULL) return ALP_ERR_IO;

	struct i3c_msg msg = {
		.buf   = (uint8_t *)data,
		.len   = (uint32_t)len,
		.flags = I3C_MSG_WRITE | I3C_MSG_STOP,
	};
	return _errno_to_alp(i3c_transfer(target, &msg, 1));
}

static alp_status_t z_read(alp_i3c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	const struct device    *dev    = (const struct device *)st->dev;
	struct i3c_device_desc *target = _resolve(dev, addr);
	if (target == NULL) return ALP_ERR_IO;

	struct i3c_msg msg = {
		.buf   = data,
		.len   = (uint32_t)len,
		.flags = I3C_MSG_READ | I3C_MSG_STOP,
	};
	return _errno_to_alp(i3c_transfer(target, &msg, 1));
}

static alp_status_t z_write_read(alp_i3c_backend_state_t *st,
                                 uint8_t                  addr,
                                 const uint8_t           *wdata,
                                 size_t                   wlen,
                                 uint8_t                 *rdata,
                                 size_t                   rlen)
{
	const struct device    *dev    = (const struct device *)st->dev;
	struct i3c_device_desc *target = _resolve(dev, addr);
	if (target == NULL) return ALP_ERR_IO;

	/* One i3c_transfer() with two chained messages (repeated START, no
	 * STOP in between) -- a two-call write-then-read would insert a
	 * STOP many targets do not tolerate. */
	struct i3c_msg msgs[2] = {
		{
		    .buf   = (uint8_t *)wdata,
		    .len   = (uint32_t)wlen,
		    .flags = I3C_MSG_WRITE,
		},
		{
		    .buf   = rdata,
		    .len   = (uint32_t)rlen,
		    .flags = I3C_MSG_RESTART | I3C_MSG_READ | I3C_MSG_STOP,
		},
	};
	return _errno_to_alp(i3c_transfer(target, msgs, 2));
}

static const alp_i3c_ops_t _ops = {
	.open       = z_open,
	.write      = z_write,
	.read       = z_read,
	.write_read = z_write_read,
	.close      = NULL, /* no teardown needed */
};

#else /* !CONFIG_I3C_CONTROLLER */

static alp_status_t
z_open(const alp_i3c_config_t *cfg, alp_i3c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)cfg;
	(void)st;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t
z_write(alp_i3c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_read(alp_i3c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_write_read(alp_i3c_backend_state_t *st,
                                 uint8_t                  addr,
                                 const uint8_t           *wdata,
                                 size_t                   wlen,
                                 uint8_t                 *rdata,
                                 size_t                   rlen)
{
	(void)st;
	(void)addr;
	(void)wdata;
	(void)wlen;
	(void)rdata;
	(void)rlen;
	return ALP_ERR_NOSUPPORT;
}

static const alp_i3c_ops_t _ops = {
	.open       = z_open,
	.write      = z_write,
	.read       = z_read,
	.write_read = z_write_read,
	.close      = NULL,
};

#endif /* CONFIG_I3C_CONTROLLER */

ALP_BACKEND_REGISTER(i3c,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
