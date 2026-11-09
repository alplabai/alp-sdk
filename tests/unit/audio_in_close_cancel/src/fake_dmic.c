/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake DMIC controller backing the alp,test-dmic DT node (see
 * ../dts/bindings/alp,test-dmic.yaml and ../boards/native_sim*.overlay).
 * The only job here is to give src/backends/audio/zephyr_drv.c's z_in_open()
 * a device that resolves and configures cleanly, so this test drives the
 * real backend's read path instead of sw_fallback.c (which never calls
 * dmic_read()).
 *
 * configure() / trigger() always succeed.  read() models a driver with no
 * data queued: it sleeps for the requested timeout (a real driver parks in
 * k_msgq_get(timeout)) and returns -EAGAIN, which is what lets the test
 * hold alp_audio_in_read() blocked in the real zephyr_drv.c backend.
 */

#define DT_DRV_COMPAT alp_test_dmic

#include <errno.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

static int fake_dmic_configure(const struct device *dev, struct dmic_cfg *config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	return 0;
}

static int fake_dmic_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cmd);
	return 0;
}

static int fake_dmic_read(const struct device *dev,
                          uint8_t              stream,
                          void               **buffer,
                          size_t              *size,
                          int32_t              timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(stream);
	ARG_UNUSED(buffer);
	ARG_UNUSED(size);
	if (timeout > 0) {
		k_msleep(timeout);
	}
	return -EAGAIN;
}

#ifdef _dmic_ops
static DEVICE_API(dmic, fake_dmic_api) = {
	.configure = fake_dmic_configure,
	.trigger   = fake_dmic_trigger,
	.read      = fake_dmic_read,
};
#else
static const struct _dmic_ops fake_dmic_api = {
	.configure = fake_dmic_configure,
	.trigger   = fake_dmic_trigger,
	.read      = fake_dmic_read,
};
#endif

static int fake_dmic_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define FAKE_DMIC_INIT(n) \
	DEVICE_DT_INST_DEFINE(n, \
	                      fake_dmic_init, \
	                      NULL, \
	                      NULL, \
	                      NULL, \
	                      POST_KERNEL, \
	                      CONFIG_AUDIO_DMIC_INIT_PRIORITY, \
	                      &fake_dmic_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_DMIC_INIT)
