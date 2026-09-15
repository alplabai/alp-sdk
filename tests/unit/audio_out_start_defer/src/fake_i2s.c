/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake I2S controller backing the alp,test-i2s DT node (see
 * ../dts/bindings/alp,test-i2s.yaml and the boards overlays). Models
 * the TX-direction behaviour of zephyr/drivers/i2s/i2s_dw.c that issue
 * #2132 is about -- unlike tests/unit/i2s_write_bounds' always-succeeds
 * fake (which never calls trigger(START) at all and so cannot catch
 * this class of bug):
 *
 *   - TRIGGER_START on an empty TX ring returns -ENOMEM (queue_get(),
 *     i2s_dw.c:144-147/794).
 *   - TRIGGER_START while already running returns -EIO (state must be
 *     READY, i2s_dw.c:279-283).
 *   - TRIGGER_STOP / TRIGGER_DRAIN while NOT running return -EIO
 *     (i2s_dw.c:301-304 / :315-321).
 *   - TRIGGER_DROP works from READY too and always releases every
 *     queued block back to the k_mem_slab captured at configure() time
 *     (tx_stream_disable() + tx_queue_drop(), i2s_dw.c:886-900),
 *     mirroring the ONE trigger the real driver accepts unconditionally.
 *   - write() queues a block regardless of run state (queue_put()); a
 *     START consumes exactly one queued block per trigger (queue_get()).
 *
 * Freeing on consume is a deliberate simplification: the real driver's
 * ISR drains the TX FIFO asynchronously and frees each block only once
 * fully sent; native_sim has no such interrupt timeline. This fake
 * instead frees a block synchronously the moment a START trigger
 * consumes it -- what these tests need to prove is that the alp-sdk
 * layer above never leaks or double-uses a block, not real-time audio
 * timing, and synchronous free-on-consume proves exactly that (a block
 * either sits in fake_i2s_tx_queue_depth() or has been returned to the
 * slab; nothing is silently stuck).
 *
 * RX is untouched: the portable audio-out / plain <alp/i2s.h> TX paths
 * this issue is about never open RX, so RX trigger()/write() always
 * succeed trivially, same as tests/unit/i2s_write_bounds' fake.
 *
 * fake_i2s_force_start_fail() pins THIS BACKEND's logic, not i2s_dw
 * fidelity (issue #2132 review, NIT). It can fail a TRIGGER_START while
 * the ring holds a block and the fake is READY-equivalent -- a state
 * real i2s_dw would answer with success (queue_get() only fails on an
 * EMPTY ring, i2s_dw.c:794-798, and the READY-state check is the only
 * OTHER way START fails, i2s_dw.c:279-283). This fake also has no
 * ERROR/underrun state and accepts write() in any state, unlike
 * i2s_dw_write() (i2s_dw.c:390-394). Tests using the force-fail knob
 * are exercising this backend's retry-and-DROP bookkeeping under an
 * injected fault, standing in for whatever real cause (a genuine
 * hardware fault, a clock glitch) could make a START fail after the
 * empty-queue case is already ruled out -- they are not a claim that
 * THIS EXACT trigger sequence is reachable on real i2s_dw.
 */

#define DT_DRV_COMPAT alp_test_i2s

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#include "fake_i2s.h"

#define TX_RING_CAP 4u

struct queue_item {
	void  *block;
	size_t size;
};

static struct queue_item     tx_ring[TX_RING_CAP];
static size_t                tx_head;
static size_t                tx_len;
static bool                  tx_running;
static struct k_mem_slab    *tx_mem_slab;
static int                   forced_start_errno;
static unsigned int          forced_start_remaining;
static fake_i2s_write_hook_t write_hook;

void fake_i2s_reset(void)
{
	tx_head                = 0;
	tx_len                 = 0;
	tx_running             = false;
	tx_mem_slab            = NULL;
	forced_start_errno     = 0;
	forced_start_remaining = 0;
	write_hook             = NULL;
}

bool fake_i2s_tx_running(void)
{
	return tx_running;
}

void fake_i2s_set_write_hook(fake_i2s_write_hook_t hook)
{
	write_hook = hook;
}

size_t fake_i2s_tx_queue_depth(void)
{
	return tx_len;
}

void fake_i2s_force_start_fail(int neg_errno, unsigned int count)
{
	forced_start_errno     = neg_errno;
	forced_start_remaining = count;
}

/* Free the oldest queued block back to the slab captured at configure()
 * time -- mirrors queue_get() + k_mem_slab_free() in i2s_dw.c's ISR /
 * tx_stream_start(). */
static void tx_free_one(void)
{
	if (tx_len == 0) return;
	struct queue_item item = tx_ring[tx_head];
	tx_head                = (tx_head + 1u) % TX_RING_CAP;
	tx_len--;
	if (tx_mem_slab != NULL) k_mem_slab_free(tx_mem_slab, item.block);
}

/* Mirrors tx_queue_drop(): free every currently-queued block, in order,
 * back to the slab -- called by STOP / DRAIN / DROP alike. */
static void tx_drop_all(void)
{
	while (tx_len > 0)
		tx_free_one();
}

static int
fake_i2s_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg)
{
	ARG_UNUSED(dev);
	if (dir == I2S_DIR_TX) tx_mem_slab = cfg->mem_slab;
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
		/* Nothing here exercises RX -- always succeeds. */
		return 0;
	}

	switch (cmd) {
	case I2S_TRIGGER_START:
		/* i2s_dw.c:279-283 -- must be READY (i.e. not already running). */
		if (tx_running) return -EIO;
		if (forced_start_remaining > 0) {
			forced_start_remaining--;
			return forced_start_errno;
		}
		/* i2s_dw.c:144-147/794 -- queue_get() on an empty ring. */
		if (tx_len == 0) return -ENOMEM;
		tx_free_one();
		tx_running = true;
		return 0;

	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		/* i2s_dw.c:301-304 / :315-321 -- refuse unless RUNNING. */
		if (!tx_running) return -EIO;
		tx_drop_all();
		tx_running = false;
		return 0;

	case I2S_TRIGGER_DROP:
		/* i2s_dw.c:329-338 -- works from READY too (only NOT_READY is
		 * refused, which this fake never reaches: configure() always
		 * leaves it READY-equivalent). Releases everything queued. */
		tx_drop_all();
		tx_running = false;
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
	/* Mirrors i2s_dw_write()/queue_put(): queues regardless of run
	 * state (READY or RUNNING both accept a write -- i2s_dw.c:390-394);
	 * this fake never reaches NOT_READY/ERROR, so always accept up to
	 * the ring's capacity. */
	ARG_UNUSED(dev);
	if (tx_len >= TX_RING_CAP) return -ENOMEM; /* i2s_dw.c:172-176, ring full */
	tx_ring[(tx_head + tx_len) % TX_RING_CAP] = (struct queue_item){
		.block = mem_block,
		.size  = size,
	};
	tx_len++;

	if (write_hook != NULL) {
		/* One-shot: clear before calling so a hook that itself calls
		 * back into a write() (it shouldn't, but stay safe) doesn't
		 * recurse. See fake_i2s.h's comment for what this reproduces. */
		fake_i2s_write_hook_t hook = write_hook;
		write_hook                 = NULL;
		hook();
	}
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
