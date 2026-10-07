/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake DMIC controller for the #2134 resample test, bound through the
 * test-local alp,test-dmic node (../dts, ../boards). Behaves like a PDM whose
 * mics cannot be clocked below 32 kHz: configure() refuses any pcm_rate under
 * FAKE_DMIC_MIN_RATE with -EINVAL (what the Alif PDM driver returns for a
 * rate outside the board's mic clock range) and records what it accepted.
 * read() hands out one real block from the caller's slab, filled with a
 * constant, so the backend's decimating read path runs end to end.
 */

#define DT_DRV_COMPAT alp_test_dmic

#include <errno.h>
#include <string.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define FAKE_DMIC_MIN_RATE 32000U
#define FAKE_DMIC_LEVEL    1000

uint32_t fake_dmic_rate;
size_t   fake_dmic_block;

static struct k_mem_slab *fake_slab;

static int fake_dmic_configure(const struct device *dev, struct dmic_cfg *config)
{
	ARG_UNUSED(dev);
	if (config->streams[0].pcm_rate < FAKE_DMIC_MIN_RATE) {
		return -EINVAL;
	}
	fake_dmic_rate  = config->streams[0].pcm_rate;
	fake_dmic_block = config->streams[0].block_size;
	fake_slab       = config->streams[0].mem_slab;
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
	ARG_UNUSED(timeout);

	void *block;

	if (k_mem_slab_alloc(fake_slab, &block, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}
	for (size_t i = 0; i < fake_dmic_block / sizeof(int16_t); i++) {
		((int16_t *)block)[i] = FAKE_DMIC_LEVEL;
	}
	*buffer = block;
	*size   = fake_dmic_block;
	return 0;
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
