/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake I2S controller backing the alp,test-i2s DT node (see
 * ../dts/bindings/alp,test-i2s.yaml and the boards overlays). Models ONE
 * specific real-hardware trap instead of the always-succeeds shape
 * tests/unit/i2s_write_bounds/src/fake_i2s.c uses -- that shape is why
 * issue #2132 shipped without a test catching it:
 *
 *   zephyr/drivers/i2s/i2s_dw.c's tx_stream_start() dequeues a block from
 *   its TX ring buffer at I2S_TRIGGER_START time and returns -ENOMEM if
 *   the buffer is empty (queue_get(), i2s_dw.c:144-147). STOP/DRAIN
 *   likewise refuse a stream that never reached I2S_STATE_RUNNING
 *   (-EIO, i2s_dw.c:301-304 / :315-321).
 *
 * write() queues a block (mirrors i2s_dw.c's queue_put(), which appends
 * regardless of run state); a TX START trigger consumes one queued block
 * per call (mirrors queue_get()). RX is untouched -- this fake only
 * models the TX-direction trap #2132 is about; the portable audio-out
 * backend under test (src/backends/audio/zephyr_drv.c) never opens RX.
 *
 * This is a NEW, test-local fake, not an edit to the shared
 * tests/unit/i2s_write_bounds fake: that test never calls trigger(START)
 * at all, but giving it this stricter behavior instead of adding a
 * second fake would still couple two tests with different intents to one
 * file, so this is the opt-in-by-separate-file choice, not an opt-in
 * Kconfig on a shared fake.
 */

#define DT_DRV_COMPAT alp_test_i2s

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#include "fake_i2s.h"

static uint32_t tx_queued;        /* blocks queued but not yet consumed by START */
static bool     tx_running;       /* mirrors i2s_dw.c's I2S_STATE_RUNNING */
static bool     force_start_fail; /* one-shot fault injection */
static int      forced_start_errno;

void fake_i2s_reset(void)
{
	tx_queued          = 0;
	tx_running         = false;
	force_start_fail   = false;
	forced_start_errno = 0;
}

bool fake_i2s_tx_running(void)
{
	return tx_running;
}

void fake_i2s_force_start_fail(int neg_errno)
{
	force_start_fail   = true;
	forced_start_errno = neg_errno;
}

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

	if (dir != I2S_DIR_TX) {
		/* alp_audio_out's I2S handle is TX-only; nothing here exercises
		 * RX, so it always succeeds. */
		return 0;
	}

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (force_start_fail) {
			force_start_fail = false;
			return forced_start_errno;
		}
		/* i2s_dw.c:794, queue_get() on an empty ring -- i2s_dw.c:144-147. */
		if (tx_queued == 0) return -ENOMEM;
		tx_queued--;
		tx_running = true;
		return 0;
	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		/* i2s_dw.c:301-304 / :315-321 -- refuses a trigger on a stream
		 * that isn't I2S_STATE_RUNNING. */
		if (!tx_running) return -EIO;
		tx_running = false;
		tx_queued  = 0;
		return 0;
	default:
		return 0;
	}
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
	/* Mirrors i2s_dw.c's queue_put(): appends to the TX ring buffer
	 * regardless of whether the stream is running yet -- write-before-
	 * start is legal (issue #2132). */
	ARG_UNUSED(dev);
	ARG_UNUSED(mem_block);
	ARG_UNUSED(size);
	tx_queued++;
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
