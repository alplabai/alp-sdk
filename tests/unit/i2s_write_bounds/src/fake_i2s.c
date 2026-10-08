/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake I2S controller backing the alp,test-i2s DT node (see
 * ../dts/bindings/alp,test-i2s.yaml and ../boards/native_sim.overlay).
 * The only job here is to give src/backends/i2s/zephyr_drv.c's z_open()
 * a device that resolves and configures cleanly, so the write-bounds
 * ztest exercises the real backend instead of tripping ALP_ERR_NOT_READY
 * on a NULL device (which is what happens on plain native_sim, and is
 * why the original #1619 regression test always skipped).
 *
 * configure() / trigger() always succeed.  write() accepts any block --
 * the alp-sdk guard under test (zephyr_drv.c:221) runs before a write
 * ever reaches this driver, so there is nothing left for the fake to
 * reject.  read() is unsupported; this test only exercises the TX path.
 */

#define DT_DRV_COMPAT alp_test_i2s

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

static int
fake_i2s_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dir);
	ARG_UNUSED(cfg);
	return 0;
}

static const struct i2s_config *fake_i2s_config_get(const struct device *dev, enum i2s_dir dir)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dir);
	return NULL;
}

static int fake_i2s_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dir);
	ARG_UNUSED(cmd);
	return 0;
}

static int fake_i2s_read(const struct device *dev, void **mem_block, size_t *size)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(mem_block);
	ARG_UNUSED(size);
	return -ENOTSUP;
}

static int fake_i2s_write(const struct device *dev, void *mem_block, size_t size)
{
	/* The alp-sdk backend under test owns the slab and its bounds
	 * check; this fake never inspects mem_block/size, it only proves
	 * the write reached the real zephyr_drv path. */
	ARG_UNUSED(dev);
	ARG_UNUSED(mem_block);
	ARG_UNUSED(size);
	return 0;
}

static const struct i2s_driver_api fake_i2s_driver_api = {
	.configure  = fake_i2s_configure,
	.config_get = fake_i2s_config_get,
	.trigger    = fake_i2s_trigger,
	.read       = fake_i2s_read,
	.write      = fake_i2s_write,
};

static int fake_i2s_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define FAKE_I2S_INIT(n) \
	DEVICE_DT_INST_DEFINE(n, \
	                      fake_i2s_init, \
	                      NULL, \
	                      NULL, \
	                      NULL, \
	                      POST_KERNEL, \
	                      CONFIG_I2S_INIT_PRIORITY, \
	                      &fake_i2s_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_I2S_INIT)
