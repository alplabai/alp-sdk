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
 *   - TRIGGER_START on an empty TX ring returns -ENOMEM, mirroring
 *     tx_stream_start()'s own queue_get() on an empty ring.
 *   - TRIGGER_START while already running returns -EIO, mirroring
 *     i2s_dw_trigger()'s START case (state must be READY).
 *   - TRIGGER_STOP / TRIGGER_DRAIN while NOT running return -EIO,
 *     mirroring i2s_dw_trigger()'s STOP and DRAIN cases.
 *   - TRIGGER_DROP works from READY too and always releases every
 *     queued block back to the k_mem_slab captured at configure() time,
 *     mirroring i2s_dw_trigger()'s DROP case (tx_stream_disable() +
 *     tx_queue_drop()) -- the ONE trigger the real driver accepts
 *     unconditionally.
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
 * RX (issue #2137): modeled with the SAME shape as TX -- a single
 * "currently filling" active block allocated at START (mirrors
 * rx_stream_start() allocating its own buffer from the slab) plus a
 * small ring of COMPLETED blocks read() drains from. Unlike TX, RX has
 * no caller-supplied ring to refuse an empty START against -- START
 * only fails if already RUNNING or in ERROR. fake_i2s_rx_simulate_
 * overrun() is this fake's only way into RX I2S_STATE_ERROR (there is
 * no real IRQ timeline here to reach it asynchronously, same caveat as
 * the TX underrun knob below) and models the i2s_dw.c fix this issue
 * ships alongside (zephyr/drivers/i2s/i2s_dw.c's RX IRQ handler freeing
 * the just-filled block on BOTH its error exits instead of leaking it)
 * -- the active block is freed back to the slab here, not leaked,
 * matching the FIXED driver.
 *
 * fake_i2s_force_start_fail() / fake_i2s_force_prepare_fail() /
 * fake_i2s_force_write_fail() pin THIS BACKEND's logic, not i2s_dw
 * fidelity. They can fail a trigger/write in states real i2s_dw would
 * answer with success (e.g. TRIGGER_START while the ring holds a block
 * and the fake is READY-equivalent -- queue_get() only fails on an
 * EMPTY ring, and the READY-state check is the only OTHER way START
 * fails). Tests using any force-fail knob are exercising this backend's
 * retry/DROP/PREPARE bookkeeping under an injected fault, standing in
 * for whatever real cause (a genuine hardware fault, a clock glitch)
 * could make a trigger/write fail after its normal precondition is
 * already ruled out -- they are not a claim that THIS EXACT sequence is
 * reachable on real i2s_dw.
 *
 * I2S_STATE_ERROR (issue #2137): fake_i2s_tx_simulate_underrun() /
 * fake_i2s_rx_simulate_overrun() are the ONLY ways in (there is no real
 * IRQ timeline here to reach it asynchronously -- see each function's
 * own comment); TRIGGER_PREPARE and TRIGGER_DROP are the only ways out.
 * From ERROR: START -EIOs (needs READY), STOP/DRAIN -EIO (need
 * RUNNING), TX write() -EIOs (needs RUNNING or READY) -- all modeled
 * below.
 */

#define DT_DRV_COMPAT alp_test_i2s

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#include "fake_i2s.h"

#define TX_RING_CAP 4u
#define RX_RING_CAP 4u

struct queue_item {
	void  *block;
	size_t size;
};

static struct queue_item     tx_ring[TX_RING_CAP];
static size_t                tx_head;
static size_t                tx_len;
static bool                  tx_running;
static bool                  tx_error; /* I2S_STATE_ERROR, issue #2137 */
static struct k_mem_slab    *tx_mem_slab;
static int                   forced_start_errno;
static unsigned int          forced_start_remaining;
static int                   forced_prepare_errno;
static unsigned int          forced_prepare_remaining;
static int                   forced_write_errno;
static unsigned int          forced_write_remaining;
static int                   forced_drop_errno;
static unsigned int          forced_drop_remaining;
static unsigned int          prepare_call_count;
static unsigned int          drain_call_count;
static unsigned int          drop_call_count;
static fake_i2s_write_hook_t write_hook;
static fake_i2s_write_hook_t write_fail_hook;

/* RX (issue #2137) -- see the file header comment for the shape. */
static struct queue_item  rx_ring[RX_RING_CAP];
static size_t             rx_head;
static size_t             rx_len;
static bool               rx_running;
static bool               rx_error;
static void              *rx_active_block;
static size_t             rx_block_bytes;
static struct k_mem_slab *rx_mem_slab;

void fake_i2s_reset(void)
{
	tx_head                  = 0;
	tx_len                   = 0;
	tx_running               = false;
	tx_error                 = false;
	tx_mem_slab              = NULL;
	forced_start_errno       = 0;
	forced_start_remaining   = 0;
	forced_prepare_errno     = 0;
	forced_prepare_remaining = 0;
	forced_write_errno       = 0;
	forced_write_remaining   = 0;
	forced_drop_errno        = 0;
	forced_drop_remaining    = 0;
	prepare_call_count       = 0;
	drain_call_count         = 0;
	drop_call_count          = 0;
	write_hook               = NULL;
	write_fail_hook          = NULL;

	rx_head         = 0;
	rx_len          = 0;
	rx_running      = false;
	rx_error        = false;
	rx_active_block = NULL;
	rx_block_bytes  = 0;
	rx_mem_slab     = NULL;
}

bool fake_i2s_tx_running(void)
{
	return tx_running;
}

bool fake_i2s_tx_in_error(void)
{
	return tx_error;
}

void fake_i2s_set_write_hook(fake_i2s_write_hook_t hook)
{
	write_hook = hook;
}

void fake_i2s_set_write_fail_hook(fake_i2s_write_hook_t hook)
{
	write_fail_hook = hook;
}

void fake_i2s_tx_simulate_underrun(void)
{
	if (tx_running && tx_len == 0) {
		tx_running = false;
		tx_error   = true;
	}
}

void fake_i2s_force_prepare_fail(int neg_errno, unsigned int count)
{
	forced_prepare_errno     = neg_errno;
	forced_prepare_remaining = count;
}

void fake_i2s_force_write_fail(int neg_errno, unsigned int count)
{
	forced_write_errno     = neg_errno;
	forced_write_remaining = count;
}

void fake_i2s_force_drop_fail(int neg_errno, unsigned int count)
{
	forced_drop_errno     = neg_errno;
	forced_drop_remaining = count;
}

size_t fake_i2s_prepare_call_count(void)
{
	return prepare_call_count;
}

size_t fake_i2s_drain_call_count(void)
{
	return drain_call_count;
}

size_t fake_i2s_drop_call_count(void)
{
	return drop_call_count;
}

size_t fake_i2s_slab_free_count(void)
{
	return tx_mem_slab != NULL ? k_mem_slab_num_free_get(tx_mem_slab) : 0u;
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

bool fake_i2s_rx_running(void)
{
	return rx_running;
}

bool fake_i2s_rx_in_error(void)
{
	return rx_error;
}

size_t fake_i2s_rx_slab_free_count(void)
{
	return rx_mem_slab != NULL ? k_mem_slab_num_free_get(rx_mem_slab) : 0u;
}

void fake_i2s_rx_simulate_overrun(void)
{
	if (!rx_running) return;
	/* issue #2137: the filled block is freed back to the slab on this
	 * exit, mirroring i2s_rx_irq_handler()'s own fix -- both its error
	 * exits (a failed k_mem_slab_alloc() for the next block, and a
	 * failed queue_put() of this one) now free the block they would
	 * otherwise have orphaned. Pre-fix, this leaked it instead (never
	 * freed, pointer simply forgotten -- not a dangling-pointer UAF, a
	 * genuine unrecoverable slab leak until close()). */
	if (rx_active_block != NULL) {
		k_mem_slab_free(rx_mem_slab, rx_active_block);
		rx_active_block = NULL;
	}
	rx_running = false;
	rx_error   = true;
}

void fake_i2s_rx_complete_block(void)
{
	/* Mirrors i2s_rx_irq_handler()'s order exactly: save the filled
	 * block, ALLOC THE NEXT ONE FIRST, and only once that succeeds
	 * attempt to hand the filled block off (queue_put()) -- this fake's
	 * ring standing in for that queue, "full" standing in for
	 * queue_put() failing. Both error exits free the block that was
	 * filled, never leaking it, matching the real driver's own #2137
	 * fix; a no-op if not genuinely RUNNING with an active block, since
	 * that state cannot legitimately complete anything. */
	if (!rx_running || rx_active_block == NULL) return;
	void *mblk_tmp = rx_active_block;

	void *blk = NULL;
	int   err = k_mem_slab_alloc(rx_mem_slab, &blk, K_NO_WAIT);
	if (err != 0) {
		/* i2s_rx_irq_handler()'s alloc-failure exit: free the block
		 * that was filled -- nothing else references it. */
		k_mem_slab_free(rx_mem_slab, mblk_tmp);
		rx_active_block = NULL;
		rx_running      = false;
		rx_error        = true;
		return;
	}
	rx_active_block = blk;

	if (rx_len >= RX_RING_CAP) {
		/* i2s_rx_irq_handler()'s queue_put()-failure exit: free the
		 * block that was filled -- the FRESH block just allocated
		 * above is freed too, mirroring rx_stream_disable()'s own
		 * generic free of stream->mem_block once the real ISR's
		 * `goto rx_disable` unwinds there. */
		k_mem_slab_free(rx_mem_slab, mblk_tmp);
		k_mem_slab_free(rx_mem_slab, rx_active_block);
		rx_active_block = NULL;
		rx_running      = false;
		rx_error        = true;
		return;
	}
	rx_ring[(rx_head + rx_len) % RX_RING_CAP] = (struct queue_item){
		.block = mblk_tmp,
		.size  = rx_block_bytes,
	};
	rx_len++;
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

/* RX equivalent of tx_drop_all() -- frees the active (in-flight) block
 * plus every completed-but-unread block, mirroring rx_stream_disable()
 * freeing stream->mem_block and DROP's own queue_drop() of anything
 * already queued for read(). */
static void rx_drop_all(void)
{
	if (rx_active_block != NULL) {
		k_mem_slab_free(rx_mem_slab, rx_active_block);
		rx_active_block = NULL;
	}
	while (rx_len > 0) {
		struct queue_item item = rx_ring[rx_head];
		rx_head                = (rx_head + 1u) % RX_RING_CAP;
		rx_len--;
		if (rx_mem_slab != NULL) k_mem_slab_free(rx_mem_slab, item.block);
	}
}

static int
fake_i2s_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg)
{
	ARG_UNUSED(dev);
	if (dir == I2S_DIR_TX) {
		tx_mem_slab = cfg->mem_slab;
	} else if (dir == I2S_DIR_RX) {
		rx_mem_slab    = cfg->mem_slab;
		rx_block_bytes = cfg->block_size;
	}
	return 0;
}

static const struct i2s_config *fake_i2s_config_get(const struct device *dev, enum i2s_dir dir)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dir);
	return NULL;
}

static int fake_i2s_tx_trigger(enum i2s_trigger_cmd cmd)
{
	switch (cmd) {
	case I2S_TRIGGER_START:
		/* Mirrors i2s_dw_trigger()'s START case -- must be READY:
		 * refuses RUNNING (already started) AND ERROR (issue #2137)
		 * alike. */
		if (tx_running || tx_error) return -EIO;
		if (forced_start_remaining > 0) {
			forced_start_remaining--;
			return forced_start_errno;
		}
		/* Mirrors tx_stream_start()'s own queue_get() on an empty
		 * ring. */
		if (tx_len == 0) return -ENOMEM;
		tx_free_one();
		tx_running = true;
		return 0;

	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		/* Mirrors i2s_dw_trigger()'s STOP and DRAIN cases -- refuse
		 * unless RUNNING: READY (never started / already stopped) and
		 * ERROR (issue #2137, post-underrun -- tx_running is already
		 * false there, so this one check covers both) both refuse. */
		drain_call_count++;
		if (!tx_running) return -EIO;
		tx_drop_all();
		tx_running = false;
		return 0;

	case I2S_TRIGGER_DROP:
		/* Mirrors i2s_dw_trigger()'s DROP case -- works from READY,
		 * RUNNING, AND ERROR alike (only NOT_READY is refused, which
		 * this fake never reaches: configure() always leaves it
		 * READY-equivalent). Releases everything queued and clears
		 * ERROR (issue #2137). Counted regardless of outcome, same
		 * rationale as prepare_call_count -- proves a caller actually
		 * ISSUED the trigger, not merely landed on a status a skipped
		 * trigger would also produce. */
		drop_call_count++;
		if (forced_drop_remaining > 0) {
			forced_drop_remaining--;
			return forced_drop_errno;
		}
		tx_drop_all();
		tx_running = false;
		tx_error   = false;
		return 0;

	case I2S_TRIGGER_PREPARE:
		/* Mirrors i2s_dw_trigger()'s PREPARE case -- the ONLY trigger
		 * valid FROM ERROR (issue #2137); refused everywhere else.
		 * Counted regardless of outcome -- fake_i2s_prepare_call_
		 * count() is how a test proves the backend actually ATTEMPTED
		 * recovery, not just that it happened to return the same
		 * status a backend with no recovery logic at all would also
		 * return. */
		prepare_call_count++;
		if (!tx_error) return -EIO;
		if (forced_prepare_remaining > 0) {
			forced_prepare_remaining--;
			return forced_prepare_errno;
		}
		/* Moves to READY and drops the ring, same as the real
		 * PREPARE case. */
		tx_drop_all();
		tx_error = false;
		return 0;

	default:
		return 0;
	}
}

static int fake_i2s_rx_trigger(enum i2s_trigger_cmd cmd)
{
	switch (cmd) {
	case I2S_TRIGGER_START:
		/* Mirrors i2s_dw_trigger()'s START case -- must be READY. */
		if (rx_running || rx_error) return -EIO;
		if (rx_mem_slab != NULL) {
			void *blk = NULL;
			/* Mirrors rx_stream_start() allocating its own buffer
			 * from the slab, unlike TX's dequeue-a-caller-filled-
			 * ring. */
			int err = k_mem_slab_alloc(rx_mem_slab, &blk, K_NO_WAIT);
			if (err != 0) return -ENOMEM;
			rx_active_block = blk;
		}
		rx_running = true;
		return 0;

	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		/* Mirrors i2s_dw_trigger()'s STOP and DRAIN cases -- refuse
		 * unless RUNNING. */
		drain_call_count++;
		if (!rx_running) return -EIO;
		rx_running = false;
		rx_drop_all();
		return 0;

	case I2S_TRIGGER_DROP:
		/* Mirrors i2s_dw_trigger()'s DROP case -- works from READY,
		 * RUNNING, AND ERROR alike; releases the active block plus
		 * anything completed but unread, and clears ERROR (issue
		 * #2137). Counted regardless of outcome -- see the TX case's
		 * own comment. */
		drop_call_count++;
		if (forced_drop_remaining > 0) {
			forced_drop_remaining--;
			return forced_drop_errno;
		}
		rx_running = false;
		rx_error   = false;
		rx_drop_all();
		return 0;

	case I2S_TRIGGER_PREPARE:
		/* Mirrors i2s_dw_trigger()'s PREPARE case -- the ONLY trigger
		 * valid FROM ERROR; refused everywhere else. The active
		 * block was already freed at the overrun that set rx_error,
		 * so there is nothing left for PREPARE's own queue_drop() to
		 * release here. */
		prepare_call_count++;
		if (!rx_error) return -EIO;
		if (forced_prepare_remaining > 0) {
			forced_prepare_remaining--;
			return forced_prepare_errno;
		}
		rx_error = false;
		return 0;

	default:
		return 0;
	}
}

static int fake_i2s_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	ARG_UNUSED(dev);

	if (dir == I2S_DIR_TX) return fake_i2s_tx_trigger(cmd);
	if (dir == I2S_DIR_RX) return fake_i2s_rx_trigger(cmd);
	/* I2S_DIR_BOTH: nothing here exercises it -- always succeeds. */
	return 0;
}

static int fake_i2s_read(const struct device *dev, void **mem_block, size_t *size)
{
	ARG_UNUSED(dev);
	if (rx_len == 0) return -EAGAIN;
	*mem_block = rx_ring[rx_head].block;
	*size      = rx_ring[rx_head].size;
	rx_head    = (rx_head + 1u) % RX_RING_CAP;
	rx_len--;
	return 0;
}

static int fake_i2s_write(const struct device *dev, void *mem_block, size_t size)
{
	/* Mirrors i2s_dw_write(): needs RUNNING or READY, refuses ERROR
	 * (issue #2137); this fake never reaches NOT_READY, so ERROR is
	 * the only other refusal to model. Checked BEFORE the forced-fail
	 * knob so a forced failure can target specifically a RETRY after a
	 * successful PREPARE already cleared tx_error -- otherwise
	 * queue_put() accepts regardless of RUNNING vs READY, up to the
	 * ring's capacity. */
	ARG_UNUSED(dev);
	if (tx_error) {
		/* One-shot hook fired from exactly the real race window
		 * z_write() has -- between THIS i2s_write() call failing
		 * -EIO and z_write()'s own PREPARE running next (both outside
		 * the backend lock at this point: this call is the caller-
		 * visible side of that unlocked i2s_write(), z_write()'s
		 * PREPARE hasn't taken the lock yet). Runs a REAL alp_i2s_*
		 * call (write/stop/start) on the SAME handle synchronously,
		 * standing in for a second thread. */
		if (write_fail_hook != NULL) {
			fake_i2s_write_hook_t hook = write_fail_hook;
			write_fail_hook            = NULL;
			hook();
		}
		return -EIO;
	}
	if (forced_write_remaining > 0) {
		forced_write_remaining--;
		return forced_write_errno;
	}
	if (tx_len >= TX_RING_CAP) return -ENOMEM; /* mirrors queue_put(), ring full */
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
