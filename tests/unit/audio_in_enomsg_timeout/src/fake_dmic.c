/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake DMIC controller backing the alp,test-dmic DT node (see
 * ../dts/bindings/alp,test-dmic.yaml and ../boards/native_sim*.overlay).
 * Mirrors tests/unit/i2s_write_bounds/src/fake_i2s.c's shape: the only job
 * here is to give src/backends/audio/zephyr_drv.c's z_in_open() a device
 * that resolves and configures cleanly, so this test exercises the real
 * backend's errno_to_alp() instead of sw_fallback.c (which never calls
 * dmic_read() at all, and would prove nothing about issue #2133 round 4d's
 * -ENOMSG override).
 *
 * configure() / trigger() always succeed. read() always returns -ENOMSG --
 * exactly what k_msgq_get(K_NO_WAIT) returns for "nothing queued yet" on a
 * non-blocking read (issue #2133 round 4d), which is what this test drives
 * through alp_audio_in_read(..., timeout_ms=0).
 */

#define DT_DRV_COMPAT alp_test_dmic

#include <errno.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>

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
	/* Always "nothing queued yet" -- this fake never actually produces a
	 * block, on purpose: the test under this only cares about the errno
	 * mapping z_in_read() applies to -ENOMSG, not real PCM delivery. */
	ARG_UNUSED(dev);
	ARG_UNUSED(stream);
	ARG_UNUSED(buffer);
	ARG_UNUSED(size);
	ARG_UNUSED(timeout);
	return -ENOMSG;
}

static const struct _dmic_ops fake_dmic_api = {
	.configure = fake_dmic_configure,
	.trigger   = fake_dmic_trigger,
	.read      = fake_dmic_read,
};

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
