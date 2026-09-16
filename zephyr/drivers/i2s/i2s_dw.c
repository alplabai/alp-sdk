/*
 * Copyright (C) 2025 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM) ======
 * The Alif Ensemble audio I2S is driven by a vendored copy of the Apache-2.0
 * zephyr_alif fork driver (drivers/i2s/i2s_dw.c, compatible
 * "snps,designware-i2s").  Upstream Zephyr v4.4 + hal_alif ship NO DesignWare
 * I2S class driver, so this is a genuine fork-driver copy carried in-tree so it
 * survives a `west update`.  Retire onto the opt-in sdk-alif fork compatible
 * once the i2s nodes are repointed AND bench-verified.  See
 * docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 *
 * FIFO/interrupt-driven (no DMA subsystem needed).  alp-sdk edits beyond this
 * provenance header cover the clock path, issue #2137's two RX ISR/recovery
 * bug fixes, and issue #2149's TX-underrun clock-keep-alive fix:
 *   - the clock path: the driver calls clock_control_set_rate() to program
 *     the I2Sx bit-clock divider off the 76.8 MHz CGU master source enabled
 *     by the Tier-1.5 clockctrl west-patch (zephyr/patches/zephyr/
 *     0001-clock_control_alif-master-source-expmst-i2s-setrate.patch), and
 *     tolerates -ENOSYS/-ENOTSUP from clock_control_configure()/set_rate()
 *     on SoCs whose clockctrl lacks those ops (e.g. native_sim);
 *   - 64-bit host builds: `mem_block_size` is `size_t`, not `uint32_t`, so it
 *     matches queue_get()'s `size_t *` parameter, and the rate argument to
 *     clock_control_set_rate() casts through `uintptr_t` before the
 *     pointer-typed clock_control_subsys_rate_t. Both are no-ops on the
 *     32-bit M55 target, where size_t and uintptr_t are already 32-bit.
 *     Without them tests/unit/i2s_dw_underrun -- the first suite to compile
 *     this driver AS-IS on native_sim/native/64 -- fails the build on
 *     -Werror=incompatible-pointer-types and -Werror=int-to-pointer-cast
 *     (issue #2149);
 *   - the RX IRQ handler's two error exits (a failed k_mem_slab_alloc() for
 *     the next block, and a failed queue_put() of the one just filled) now
 *     free the block they would otherwise have orphaned instead of leaking
 *     it (issue #2137);
 *   - rx_stream_start() now resets mem_block_offset to 0 on every (re)start,
 *     matching tx_stream_start()'s own reset -- without it, a stale offset
 *     left over by any prior stop (not only an ERROR-recovery one: none of
 *     STOP/DRAIN/DROP/PREPARE reset this field either) survived into the
 *     freshly allocated block, and the first ISR after the restart
 *     delivered it as a "complete" frame that was never actually filled
 *     (issue #2137);
 *   - i2s_tx_irq_handler()'s queue-empty underrun branch (the one that sets
 *     I2S_STATE_ERROR because queue_get() came back empty, NOT the
 *     already-ERROR or last_block exits) now leaves CER.CLKEN set instead
 *     of calling i2s_clock_disable() -- it still disables the TX
 *     channel/block/interrupt and still sets I2S_STATE_ERROR exactly as
 *     before. A codec on the other end of this bus (TAS2563 on the E1M-EVK)
 *     reads bit-clock loss as its cue to latch SHUTDOWN, and #2137's own
 *     ERROR->READY->RUNNING recovery through PREPARE + a retried write
 *     silently succeeds while the amp stays off, since nothing in that path
 *     ever re-arms it (issue #2149). Every OTHER path that tears the stream
 *     down -- STOP/DRAIN/DROP (i2s_dw_trigger()), PM suspend
 *     (i2s_disable_controller()) -- still gates the clock exactly as
 *     before, including DROP out of ERROR (the abandon-the-stream case),
 *     so a stream nobody is trying to recover from no longer leaves the
 *     clock running forever. tx_stream_start()'s own restart, in turn, now
 *     skips reprogramming clock_control_set_rate()/CCR when a one-shot flag
 *     (clk_restart_skip_ok, set ONLY by the underrun exit above and cleared
 *     by every path that can invalidate it -- see the field's own comment)
 *     confirms CER.CLKEN is already set for the exact restart the flag was
 *     armed for, instead of writing CCR live, which its own doc comment
 *     says must be done with the clock disabled.
 *     Known remaining gap, NOT fixed here: rx_stream_start() reprograms
 *     clock_control_set_rate()/CCR unconditionally on every RX start,
 *     including while the shared clock may already be running (e.g. a TX
 *     stream left it on via this very keep-clock underrun exit), and
 *     rx_stream_disable() unconditionally clears CER on every RX overrun --
 *     either one on a controller sharing this clock with an active TX
 *     stream would reprogram or kill the TX bit clock out from under it.
 *     This driver is half-duplex in practice (dev_data->dir is a single
 *     field, not per-direction), which narrows but does not eliminate the
 *     exposure: the two directions can still be reconfigured back to back
 *     on the same physical clock. Refs #2150 (tracking issue already
 *     filed; out of scope for this TX-underrun fix).
 * The register block layout, IRQ scheme, and FIFO trigger levels are the
 * fork's.
 * vendor-ext, BENCH-UNVERIFIED (compiles + links on the E8 he target; the TX
 * tone-out / clock programming were exercised on the bench as PARTIAL/PASS but
 * the achieved SCLK rate is a bench follow-up).
 */
#define DT_DRV_COMPAT snps_designware_i2s

#include <string.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <soc.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>

#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>

#include "i2s_dw.h"

#define WSS_LEN			2
#define EXT_CLK_SRC_ENABLE	0
#define TX_FIFO_TRG_LVL		8
#define RX_FIFO_TRG_LVL		8

LOG_MODULE_REGISTER(i2s_dw);

#define DMA_NUM_CHANNELS	8

struct queue_item {
	void *mem_block;
	size_t size;
};

/* Minimal ring buffer implementation */
struct dw_ring_buf {
	struct queue_item *buf;
	uint16_t len;
	uint16_t head;
	uint16_t tail;
};

struct stream {
	int32_t   state;
	struct    k_sem sem;
	uint32_t  dma_channel;
	struct dma_config dma_cfg;
	uint8_t   priority;
	bool      src_addr_increment;
	bool      dst_addr_increment;
	uint8_t   fifo_threshold;
	struct    i2s_config cfg;
	struct dw_ring_buf mem_block_queue;
	void      *mem_block;
	size_t    mem_block_size;
	uint32_t  mem_block_offset;
	/* alp-sdk issue #2149 (round 2): one-shot flag. Set ONLY by
	 * i2s_tx_irq_handler()'s queue-empty underrun exit, the single path
	 * that leaves CER.CLKEN set on purpose. tx_stream_start() consults it
	 * (paired with i2s_clock_is_enabled()) to skip reprogramming
	 * clock_control_set_rate()/CCR on a restart that immediately follows
	 * that exact exit, then consumes (clears) it unconditionally either
	 * way. Every path that can invalidate the assumption before that
	 * restart happens -- a rate change (i2s_dw_configure()),
	 * rx_stream_start()/_disable() touching the SAME shared clock, any
	 * tx_stream_disable_ex() that actually gates the clock, PM suspend,
	 * and PM resume -- also clears it, so a stale flag can never arm the
	 * skip against the wrong divider (round-2 review finding 1: the
	 * earlier clk_frame_freq_configured cache this replaces was never
	 * cleared on any of those paths). */
	bool      clk_restart_skip_ok;
	bool      last_block;
	bool      master;
	int (*stream_start)(struct stream *strm, const struct device *dev);
	void (*stream_disable)(struct stream *strm, const struct device *dev);
	void (*queue_drop)(struct stream *strm);
};

/* Device run time data */
struct i2s_dw_data {
	uint32_t irq_mask_cache;
	enum i2s_dir dir;
	struct stream rx;
	struct stream tx;
};

#define MODULO_INC(val, max) { val = (++val < max) ? val : 0; }

static int32_t i2s_configure_clocksource(bool enable,
					const struct i2s_dw_cfg *i2s,
					uint32_t sample_rate)
{
	int32_t ret = 0;
	uint32_t sclk = 0;
	const uint32_t clock_cycles[WSS_CLOCK_CYCLES_MAX] = {16, 24, 32};

	if (enable) {
		if (!sample_rate) {
			return -1;
		}

		/* Calculate sclk = 2* WSS * Sample Rate*/
		/* WSS = 32 */
		sclk = 2 * clock_cycles[i2s->cfg.wss_len] * (sample_rate);

		ret = clock_control_set_rate(i2s->clk_dev,
				i2s->clkid, (clock_control_subsys_rate_t)(uintptr_t)sclk);
		/* alp-sdk: on the Alif clockctrl the I2S bit-clock divider in
		 * CLKCTL_PER_SLV I2Sx_CTRL is now programmed from `sclk` by the
		 * clockctrl .set_rate (Tier-1.5 west-patch
		 * zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch).
		 * The divider field layout in that patch is BENCH-UNVERIFIED against the
		 * DFP/TRM, so the achieved SCLK (and thus sample rate) is not yet bench-
		 * confirmed. On native_sim / other SoCs whose clockctrl has no .set_rate
		 * this still returns -ENOSYS/-ENOTSUP, which we tolerate (same lesson as
		 * the SPI/PDM clock bring-up). */
		if (ret != 0 && ret != -ENOSYS && ret != -ENOTSUP) {
			LOG_ERR("Unable to set desired frequency : err:%d", ret);
			return ret;
		}
	}

	return 0;
}

/*
 * Get data from the queue
 */
static int queue_get(struct dw_ring_buf *rb, void **mem_block, size_t *size)
{
	unsigned int key;

	key = irq_lock();

	if (rb->tail == rb->head) {
		/* Ring buffer is empty */
		irq_unlock(key);
		return -ENOMEM;
	}

	*mem_block = rb->buf[rb->tail].mem_block;
	*size = rb->buf[rb->tail].size;
	MODULO_INC(rb->tail, rb->len);

	irq_unlock(key);

	return 0;
}

/*
 * Put data in the queue
 */
static int queue_put(struct dw_ring_buf *rb, void *mem_block, size_t size)
{
	uint16_t head_next;
	unsigned int key;

	key = irq_lock();

	head_next = rb->head;
	MODULO_INC(head_next, rb->len);

	if (head_next == rb->tail) {
		/* Ring buffer is full */
		irq_unlock(key);
		return -ENOMEM;
	}

	rb->buf[rb->head].mem_block = mem_block;
	rb->buf[rb->head].size = size;
	rb->head = head_next;

	irq_unlock(key);

	return 0;
}

static void i2s_enable_controller(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;

	/* Enable I2S */
	i2s_global_enable(i2s);

	/* Enable Master Clock */
	i2s_configure_clock(i2s);
	i2s_clock_enable(i2s);
}

static int i2s_dw_configure(const struct device *dev, enum i2s_dir dir,
			       const struct i2s_config *i2s_cfg)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;

	struct stream *stream;

	dev_data->dir = dir;

	switch (dir) {
	case I2S_DIR_RX:
		stream = &dev_data->rx;
		break;
	case I2S_DIR_TX:
		stream = &dev_data->tx;
		break;
	case I2S_DIR_BOTH:
		return -ENOSYS;
	default:
		LOG_ERR("Either RX or TX direction must be selected");
		return -EINVAL;
	}

	if (stream->state != I2S_STATE_NOT_READY &&
	    stream->state != I2S_STATE_READY) {
		LOG_ERR("invalid state");
		return -EINVAL;
	}

	/* alp-sdk issue #2149 (round 2): a rate change invalidates TX's
	 * one-shot restart-skip flag -- the divider a restart would skip
	 * reprogramming was derived from the OLD frame_clk_freq. Only the TX
	 * stream's flag is ever consulted (by tx_stream_start()), but this
	 * runs for either dir before stream->cfg is overwritten below, so it
	 * also catches the frame_clk_freq==0 "drop config" early return. */
	if (dir == I2S_DIR_TX && i2s_cfg->frame_clk_freq != stream->cfg.frame_clk_freq) {
		stream->clk_restart_skip_ok = false;
	}

	stream->master = true;
	if (i2s_cfg->options & I2S_OPT_FRAME_CLK_TARGET ||
	    i2s_cfg->options & I2S_OPT_BIT_CLK_TARGET) {
		stream->master = false;
	}

	if (i2s_cfg->frame_clk_freq == 0U) {
		stream->queue_drop(stream);
		memset(&stream->cfg, 0, sizeof(struct i2s_config));
		stream->state = I2S_STATE_NOT_READY;
		return 0;
	}

	memcpy(&stream->cfg, i2s_cfg, sizeof(struct i2s_config));

	/* Set FIFO Trigger Level */
	if (dir == I2S_DIR_TX) {
		i2s_set_tx_trigger_level(i2s);
	} else if (dir == I2S_DIR_RX) {
		i2s_set_rx_trigger_level(i2s);
	}

	stream->state = I2S_STATE_READY;
	return 0;
}

static int i2s_dw_trigger(const struct device *dev, enum i2s_dir dir,
			     enum i2s_trigger_cmd cmd)
{
	struct i2s_dw_data *const dev_data = dev->data;
	struct stream *stream;
	unsigned int key;
	int ret;

	switch (dir) {
	case I2S_DIR_RX:
		stream = &dev_data->rx;
		break;
	case I2S_DIR_TX:
		stream = &dev_data->tx;
		break;
	case I2S_DIR_BOTH:
		return -ENOSYS;
	default:
		LOG_ERR("Either RX or TX direction must be selected");
		return -EINVAL;
	}

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (stream->state != I2S_STATE_READY) {
			LOG_ERR("START trigger: invalid state %d",
				    stream->state);
			return -EIO;
		}

		__ASSERT_NO_MSG(stream->mem_block == NULL);

		ret = stream->stream_start(stream, dev);
		if (ret < 0) {
			LOG_ERR("START trigger failed %d", ret);
			return ret;
		}

		pm_device_busy_set(dev);

		stream->state = I2S_STATE_RUNNING;
		stream->last_block = false;
		break;

	case I2S_TRIGGER_STOP:
		key = irq_lock();
		if (stream->state != I2S_STATE_RUNNING) {
			irq_unlock(key);
			LOG_ERR("STOP trigger: invalid state");
			return -EIO;
		}
		irq_unlock(key);
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		stream->last_block = true;

		pm_device_busy_clear(dev);
		break;

	case I2S_TRIGGER_DRAIN:
		key = irq_lock();
		if (stream->state != I2S_STATE_RUNNING) {
			irq_unlock(key);
			LOG_ERR("DRAIN trigger: invalid state");
			return -EIO;
		}
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		irq_unlock(key);
		pm_device_busy_clear(dev);
		break;

	case I2S_TRIGGER_DROP:
		if (stream->state == I2S_STATE_NOT_READY) {
			LOG_ERR("DROP trigger: invalid state");
			return -EIO;
		}
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		pm_device_busy_clear(dev);
		break;

	case I2S_TRIGGER_PREPARE:
		if (stream->state != I2S_STATE_ERROR) {
			LOG_ERR("PREPARE trigger: invalid state");
			return -EIO;
		}
		stream->state = I2S_STATE_READY;
		stream->queue_drop(stream);
		break;

	default:
		LOG_ERR("Unsupported trigger command");
		return -EINVAL;
	}

	return 0;
}

static int i2s_dw_read(const struct device *dev, void **mem_block,
			  size_t *size)
{
	struct i2s_dw_data *const dev_data = dev->data;
	int ret;

	if (dev_data->rx.state == I2S_STATE_NOT_READY) {
		LOG_DBG("invalid state");
		return -EIO;
	}
	if (dev_data->rx.state != I2S_STATE_ERROR) {
		ret = k_sem_take(&dev_data->rx.sem,
				 SYS_TIMEOUT_MS(dev_data->rx.cfg.timeout));
		if (ret < 0) {
			return ret;
		}
	}

	/* Get data from the beginning of RX queue */
	ret = queue_get(&dev_data->rx.mem_block_queue, mem_block, size);
	if (ret < 0) {
		return -EIO;
	}

	return 0;
}

static int i2s_dw_write(const struct device *dev, void *mem_block,
			   size_t size)
{
	struct i2s_dw_data *const dev_data = dev->data;
	int ret;

	if (dev_data->tx.state != I2S_STATE_RUNNING &&
	    dev_data->tx.state != I2S_STATE_READY) {
		LOG_DBG("invalid state");
		return -EIO;
	}

	ret = k_sem_take(&dev_data->tx.sem,
			 SYS_TIMEOUT_MS(dev_data->tx.cfg.timeout));
	if (ret < 0) {
		return ret;
	}

	/* Add data to the end of the TX queue */
	queue_put(&dev_data->tx.mem_block_queue, mem_block, size);

	return 0;
}

static const struct i2s_driver_api i2s_dw_driver_api = {
	.configure = i2s_dw_configure,
	.read = i2s_dw_read,
	.write = i2s_dw_write,
	.trigger = i2s_dw_trigger,
};

static void tx_stream_disable(struct stream *stream, const struct device *dev);
static void tx_stream_disable_ex(struct stream *stream, const struct device *dev, bool keep_clock);
static void rx_stream_disable(struct stream *stream, const struct device *dev);

static void i2s_tx_irq_handler(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	struct stream *stream = &dev_data->tx;
	const uint32_t num_channels = stream->cfg.format & I2S_FMT_DATA_FORMAT_MASK
				      ? 2U : stream->cfg.channels;
	/* Number of data that can copy to TX FIFO */
	uint32_t tx_avail = I2S_FIFO_DEPTH - i2s->cfg.tx_fifo_trg_lvl;
	const uint8_t *buff = stream->mem_block; /* Assign the buffer base address */
	uint8_t last_lap = 0, bytes = 0, cnt = 0, frames = 0;
	uint32_t offset = stream->mem_block_offset;
	size_t size = stream->mem_block_size;
	/* alp-sdk issue #2149: set true ONLY on the queue-empty underrun exit
	 * below. Every other tx_disable entry (already-ERROR, last_block)
	 * keeps disabling the clock exactly as before. */
	bool keep_clock = false;
	int ret;

	/* Stop transmission if there was an error */
	if (stream->state == I2S_STATE_ERROR) {
		LOG_ERR("TX error detected");
		goto tx_disable;
	}

	/* Stop transmission if we were requested */
	if (stream->last_block) {
		stream->state = I2S_STATE_READY;
		goto tx_disable;
	}

	if (stream->cfg.word_size <= 16) {
		bytes = I2S_16BIT_BUF_TYPE;
	} else {
		bytes = I2S_32BIT_BUF_TYPE;
	}

	/* Check if it is the last lap */
	if ((offset + (2 * tx_avail * bytes)) >  size) {
		/* Assign the number of iterations required */
		frames = (size - offset)/(2*bytes);
		last_lap = 1;
	} else {
		frames = tx_avail;
	}

	for (cnt = 0; cnt < frames; cnt++) {
		/* Assuming that application uses 16bit buffer for 16bit data resolution */
		if (bytes == I2S_16BIT_BUF_TYPE) {
			if (num_channels == 1) {
				i2s_write_left_tx((uint32_t)
				(*(uint16_t *)(buff + offset)), i2s);
				i2s_write_right_tx(0, i2s);
				offset = offset + I2S_16BIT_BUF_TYPE;
			} else {
				i2s_write_left_tx((uint32_t)
				(*(uint16_t *)(buff + offset)), i2s);
				i2s_write_right_tx((uint32_t) (*(uint16_t *)
				(buff + offset + I2S_16BIT_BUF_TYPE)), i2s);
				offset = offset + 2 * I2S_16BIT_BUF_TYPE;
			}
		} else {
			/* For > 16bit data resolution */
			/* consider as 32bit buffer */
			if (num_channels == 1) {
				i2s_write_left_tx(*(uint32_t *)(buff + offset), i2s);
				i2s_write_right_tx(0, i2s);
				offset = offset + I2S_32BIT_BUF_TYPE;
			} else {
				i2s_write_left_tx(*(uint32_t *)(buff + offset), i2s);
				i2s_write_right_tx(*(uint32_t *)
					(buff + offset + I2S_32BIT_BUF_TYPE), i2s);
				offset = offset + (2 * I2S_32BIT_BUF_TYPE);
			}
		}
	}

	if (last_lap && (offset < size)) {
		if (num_channels == 1) {
			/* Write the Left sample and fill right with 0 */
			i2s_write_left_tx((uint32_t)
					(*(uint16_t *)(buff + offset)), i2s);
			i2s_write_right_tx(0, i2s);
			offset = offset + I2S_16BIT_BUF_TYPE;
		} else {
			/* Write the Left sample and fill right with 0 */
			i2s_write_left_tx(*(uint32_t *)(buff + offset), i2s);
			i2s_write_right_tx(0, i2s);
			offset = offset + I2S_32BIT_BUF_TYPE;
		}
	}

	stream->mem_block_offset = offset;

	/* Send complete event once all the data is copied to FIFO */
	if (offset >= size) {
		/* All block data sent */
		k_mem_slab_free(stream->cfg.mem_slab, stream->mem_block);
		stream->mem_block = NULL;
		stream->mem_block_offset = 0;

		/* Prepare to send the next data block */
		ret = queue_get(&stream->mem_block_queue, &stream->mem_block,
				&stream->mem_block_size);
		if (ret < 0) {
			if (stream->state == I2S_STATE_STOPPING) {
				stream->state = I2S_STATE_READY;
			} else {
				stream->state = I2S_STATE_ERROR;
				/* alp-sdk issue #2149: this is the underrun exit --
				 * the queue genuinely ran dry mid-playback, not a
				 * caller-requested stop/drain/drop. Keep CER.CLKEN
				 * set so the codec on the other end of this I2S bus
				 * never sees bit-clock loss and never latches its
				 * own SHUTDOWN; the TX channel/block/interrupt are
				 * still disabled below exactly as before. */
				keep_clock = true;
				/* alp-sdk issue #2149 (round 2): arm the
				 * one-shot restart-skip flag ONLY here -- the
				 * only place CER.CLKEN is deliberately left
				 * set. See struct stream's own comment. */
				stream->clk_restart_skip_ok = true;
			}
			goto tx_disable;
		}
		k_sem_give(&stream->sem);
	}
	return;

tx_disable:
	tx_stream_disable_ex(stream, dev, keep_clock);
}

static void i2s_rx_irq_handler(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	struct stream *stream = &dev_data->rx;
	void *mblk_tmp;
	int ret;

	 /* Data available in RX FIFO */
	uint32_t rx_avail = i2s->cfg.rx_fifo_trg_lvl;
	uint8_t last_lap = 0, bytes = 0, cnt = 0, frames = 0;
	const uint32_t num_channels =
			stream->cfg.format & I2S_FMT_DATA_FORMAT_MASK
			? 2U : stream->cfg.channels;
	/* Assign the buffer base address */
	uint8_t *const buff  = stream->mem_block;
	uint32_t offset = stream->mem_block_offset;
	uint32_t size = stream->cfg.block_size;

	/* Stop reception if there was an error */
	if (stream->state == I2S_STATE_ERROR) {
		goto rx_disable;
	}
	/* Stop reception if we were requested */
	if (stream->state == I2S_STATE_STOPPING) {
		stream->state = I2S_STATE_READY;
		goto rx_disable;
	}

	if ((stream->cfg.word_size <= 16)) {
		bytes = I2S_16BIT_BUF_TYPE;
	} else {
		bytes = I2S_32BIT_BUF_TYPE;
	}
	/* Check if it is the last lap */
	if ((offset + (2 * rx_avail * bytes)) >  size) {
		/* Assign the number of iterations required */
		frames = (size - offset)/(2*bytes);
		last_lap = 1;
	} else {
		frames = rx_avail;
	}

	for (cnt = 0; cnt < frames; cnt++) {
		/* Assuming that application uses 16bit */
		/* buffer for 16bit data resolution */
		if (bytes == I2S_16BIT_BUF_TYPE) {
			if (num_channels == 1) {
				(*(uint16_t *)(buff + offset)) =
				(uint16_t)i2s_read_left_rx(i2s);
				i2s_read_right_rx(i2s);
				offset = offset + I2S_16BIT_BUF_TYPE;
			} else {
				(*(uint16_t *)(buff + offset)) =
				(uint16_t)i2s_read_left_rx(i2s);
				(*(uint16_t *)
				(buff + offset + I2S_16BIT_BUF_TYPE)) =
				(uint16_t)i2s_read_right_rx(i2s);
				offset = offset + 2 * I2S_16BIT_BUF_TYPE;
			}
		} else {
			/* For > 16bit data resolution consider */
			/* as 32bit buffer */
			if (num_channels == 1) {
				*(uint32_t *)(buff + offset) = i2s_read_left_rx(i2s);
				i2s_read_right_rx(i2s);
				offset = offset + I2S_32BIT_BUF_TYPE;
			} else {
				*(uint32_t *)(buff + offset) = i2s_read_left_rx(i2s);
				*(uint32_t *)(buff + offset + I2S_32BIT_BUF_TYPE) =
				i2s_read_right_rx(i2s);
				offset = offset + 2 * I2S_32BIT_BUF_TYPE;
			}
		}
	}

	if (last_lap && (offset < size)) {
		if (bytes == I2S_16BIT_BUF_TYPE) {
			/* Read the last sample from left */
			(*(uint16_t *)(buff + offset)) =
			(uint16_t)i2s_read_left_rx(i2s);
			i2s_read_right_rx(i2s);
			offset = offset + I2S_16BIT_BUF_TYPE;
		} else {
			/* Read the last sample from left */
			*(uint32_t *)(buff + offset) = i2s_read_left_rx(i2s);
			i2s_read_right_rx(i2s);
			offset = offset + I2S_32BIT_BUF_TYPE;
		}
	}

	stream->mem_block_offset = offset;

	/* Once the buffer is full, */
	/* send complete event with interrupt disabled */
	if (offset >= size) {
		mblk_tmp = stream->mem_block;

		/* Prepare to receive the next data block */
		ret = k_mem_slab_alloc(stream->cfg.mem_slab,
				       &stream->mem_block,
				       K_NO_WAIT);
		if (ret < 0) {
			/* alp-sdk issue #2137: k_mem_slab_alloc() sets
			 * stream->mem_block to NULL on this path, so
			 * rx_stream_disable()'s own free (it only frees
			 * stream->mem_block) never sees mblk_tmp -- the
			 * just-filled block this ISR was about to hand off.
			 * Free it here or it leaks every overrun, and with
			 * the 2-block slab alp-sdk's backend allocates
			 * (src/backends/i2s/zephyr_drv.c) two overruns starve
			 * the slab until close(). */
			stream->state = I2S_STATE_ERROR;
			k_mem_slab_free(stream->cfg.mem_slab, mblk_tmp);
			goto rx_disable;
		}
		stream->mem_block_offset = 0;

		/* All block data received */
		ret = queue_put(&stream->mem_block_queue, mblk_tmp,
				stream->cfg.block_size);
		if (ret < 0) {
			/* alp-sdk issue #2137: queue_put() failing leaves
			 * mblk_tmp neither queued nor referenced by
			 * stream->mem_block (which the alloc above already
			 * replaced) -- same leak as above, same fix. */
			stream->state = I2S_STATE_ERROR;
			k_mem_slab_free(stream->cfg.mem_slab, mblk_tmp);
			goto rx_disable;
		}
		k_sem_give(&stream->sem);
	}
	return;

rx_disable:
	rx_stream_disable(stream, dev);
}

static void i2s_dw_isr(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	uint32_t int_status = 0;

	/* Get the Current Interrupt Status*/
	int_status = i2s->paddr->ISR;

	if ((dev_data->dir == I2S_DIR_TX) &&
	    (_FLD2VAL(I2S_ISR_TXFE, int_status))) {
		/* Handle Tx Interrupt */
		i2s_tx_irq_handler(dev);
	}
	if ((dev_data->dir == I2S_DIR_RX) &&
	    (_FLD2VAL(I2S_ISR_RXDA, int_status))) {
		/* Handle Rx Interrupt */
		i2s_rx_irq_handler(dev);
	}

	/* This should not happen */
	if (_FLD2VAL(I2S_ISR_TXFO, int_status)) {
		i2s_clear_tx_overrun(i2s);
	}

	if (_FLD2VAL(I2S_ISR_RXFO, int_status)) {
		/* Clear overrun interrupt */
		i2s_clear_rx_overrun(i2s);

		/* Disable the Rx Overflow interrupt for now. This will */
		/* be enabled again when Receive function is called */
		i2s_disable_rx_fo_interrupt(i2s);
	}

}

static int i2s_dw_initialize(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	int ret;

#if defined(CONFIG_PINCTRL)
	if (i2s->pincfg != NULL) {
		ret = pinctrl_apply_state(i2s->pincfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			LOG_ERR("I2S pinctrl setup failed (%d)", ret);
			return ret;
		}
	}
#endif

	/* check device availability */
	if (!device_is_ready(i2s->clk_dev)) {
		LOG_ERR("clock controller device not ready");
		return -ENODEV;
	}

	/* Configure I2S clock sources */
	ret = clock_control_configure(i2s->clk_dev,
			i2s->clkid, NULL);
	/* alp-sdk: tolerate -ENOSYS/-ENOTSUP -- the upstream Alif clockctrl no-ops
	 * .configure() (same as SPI/PDM). The fork's PM-resume path (further down)
	 * already tolerates this; the init path did not. The clock gate itself is
	 * applied by clock_control_on() below. */
	if (ret != 0 && ret != -ENOSYS && ret != -ENOTSUP) {
		LOG_ERR("Unable to configure clock: err:%d", ret);
		return ret;
	}

	/* Enable I2S clock from clock manager */
	ret = clock_control_on(i2s->clk_dev, i2s->clkid);
	if (ret != 0) {
		LOG_ERR("Unable to turn on clock: err:%d", ret);
		return ret;
	}

	/* Enable and configure the I2S controller */
	i2s_enable_controller(dev);

	i2s->irq_config(dev);

	k_sem_init(&dev_data->rx.sem, 0, CONFIG_I2S_DW_RX_BLOCK_COUNT);
	k_sem_init(&dev_data->tx.sem, CONFIG_I2S_DW_TX_BLOCK_COUNT,
		   CONFIG_I2S_DW_TX_BLOCK_COUNT);

	/* Mask all the interrupts */
	i2s_disable_tx_interrupt(i2s);
	i2s_disable_rx_interrupt(i2s);
	LOG_INF("%s inited", dev->name);

	return 0;
}

static int rx_stream_start(struct stream *stream, const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	int ret;

	ret = k_mem_slab_alloc(stream->cfg.mem_slab, &stream->mem_block,
			       K_NO_WAIT);
	if (ret < 0) {
		return ret;
	}
	/* alp-sdk issue #2137: mirrors tx_stream_start()'s own reset for the
	 * SAME reason. None of STOP/DRAIN/DROP/PREPARE reset this field, so
	 * a stale "already full" offset from a previous block survives into
	 * whatever fresh stream->mem_block this call allocates -- not only
	 * on the ERROR-recovery path (PREPARE moves ERROR->READY without
	 * touching it, and the overrun ISR exit that set it to the full
	 * block size never got to reset it either), but also on a plain
	 * stop() mid-block followed by a fresh start(): none of the trigger
	 * cases that can precede this call ever clear mem_block_offset.
	 * Without this reset, the first ISR firing after any such restart
	 * sees offset already >= size, computes frames=0 for a block that
	 * was never actually filled, and queues it as a "complete" frame on
	 * the strength of a stale offset alone -- stale/garbage sample data
	 * delivered silently, on literally the first frame after the
	 * restart. */
	stream->mem_block_offset = 0;

	/* alp-sdk issue #2149 (round 2): RX reprograms the SAME shared
	 * bit-clock divider below (i2s_configure_clocksource() +
	 * i2s_configure_clock()) unconditionally, regardless of whether the
	 * clock is already running -- e.g. a TX stream may have left it on
	 * via this fix's own keep-clock underrun exit. Invalidate TX's
	 * one-shot skip flag here or a TX restart that follows this RX start
	 * could wrongly skip reprogramming onto RX's divider (round-2 review
	 * finding 1). */
	dev_data->tx.clk_restart_skip_ok = false;

	/* Configure the I2S Peripheral Clock */
	i2s_configure_clocksource(true, i2s, stream->cfg.frame_clk_freq);

	/* Reset the Rx FIFO */
	i2s_rx_fifo_reset(i2s);
	/* Set WLEN */
	i2s_rx_config_wlen(i2s, stream->cfg.word_size);
	/* Enable Master Clock */
	i2s_configure_clock(i2s);
	i2s_clock_enable(i2s);
	/* Disable Tx Channel */
	i2s_tx_channel_disable(i2s);

	/* Clear Overrun interrupt if any */
	i2s_clear_rx_overrun(i2s);

	/* Enable Rx Channel */
	i2s_rx_channel_enable(i2s);

	/* Enable RX Interrupts */
	i2s_enable_rx_interrupt(i2s);
	/* Enable Rx Block */
	i2s_rx_block_enable(i2s);

	return 0;
}

static int tx_stream_start(struct stream *stream, const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	int ret;
	/* alp-sdk issue #2149: a restart following the ISR underrun path
	 * (i2s_tx_irq_handler() -> PREPARE -> here) can find CER.CLKEN
	 * already set -- deliberately, since the underrun exit is now the
	 * one case that leaves it that way on purpose. i2s_configure_clock()
	 * below programs CCR (SCLKG/WSS), and its own doc comment (i2s_dw.h)
	 * says that must be done "with Clock disabled"; note CLKEN is
	 * already 1 from i2s_dw_initialize() (i2s_enable_controller() ->
	 * i2s_clock_enable()) onward, so in practice CCR has ALWAYS been
	 * written with the clock already live on every first start since
	 * boot, not only on this restart path -- this comment used to imply
	 * otherwise (round-2 review finding 5). What actually changes here is
	 * narrower: avoid RE-touching the divider during underrun recovery
	 * specifically, since re-deriving the same divider gains nothing and
	 * a live CCR rewrite is not something to do without silicon proof
	 * it's glitch-free. Chosen: skip both clock_control_set_rate() and
	 * i2s_configure_clock() on a restart that immediately follows the
	 * ISR's own keep-clock underrun exit -- tracked by the one-shot
	 * clk_restart_skip_ok flag (round-2 review finding 1; NOT a
	 * remembered sample rate, which was the earlier, buggier form of
	 * this guard -- see the field's own comment), consumed/cleared on
	 * this very call so a stale flag can never arm the skip on an
	 * unrelated restart. i2s->cfg.wss_len/sclkg (CCR's other inputs) are
	 * fixed device-config constants no i2s_dw_configure() call ever
	 * changes, so a rate change is exactly what clears the flag (see
	 * i2s_dw_configure()) -- the flag being set already implies
	 * frame_clk_freq is unchanged.
	 * BENCH-VERIFY: confirm no BCLK/WS glitch on the restart path that
	 * DOES still reprogram (a genuine rate change between PREPARE and
	 * this START, not exercised by the #2137 recovery flow). */
	bool clk_needs_reprogram;

	ret = queue_get(&stream->mem_block_queue, &stream->mem_block,
			&stream->mem_block_size);
	if (ret < 0) {
		return ret;
	}
	k_sem_give(&stream->sem);

	stream->mem_block_offset = 0;

	clk_needs_reprogram = !stream->clk_restart_skip_ok || !i2s_clock_is_enabled(i2s);
	/* alp-sdk issue #2149 (round 2): the flag is one-shot -- consume
	 * (clear) it on this restart regardless of which branch below
	 * actually runs. Only another queue-empty underrun exit re-arms it. */
	stream->clk_restart_skip_ok = false;

	if (clk_needs_reprogram) {
		/* Configure the I2S Peripheral Clock */
		i2s_configure_clocksource(true, i2s, stream->cfg.frame_clk_freq);
	}

	/* Reset the Tx FIFO */
	i2s_tx_fifo_reset(i2s);

	/* Set WLEN */
	i2s_tx_config_wlen(i2s, stream->cfg.word_size);

	/* Enable Master Clock */
	if (clk_needs_reprogram) {
		i2s_configure_clock(i2s);
	}
	i2s_clock_enable(i2s);

	/* Disable Rx Channel */
	i2s_rx_channel_disable(i2s);

	/* Clear Overrun interrupt if any */
	i2s_clear_tx_overrun(i2s);

	/* Enable Tx Channel */
	i2s_tx_channel_enable(i2s);

	/* Enable Tx Interrupt */
	i2s_enable_tx_interrupt(i2s);

	/* Enable Tx Block */
	i2s_tx_block_enable(i2s);

	return 0;
}

static void rx_stream_disable(struct stream *stream, const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;

	if (stream->mem_block != NULL) {
		k_mem_slab_free(stream->cfg.mem_slab, stream->mem_block);
		stream->mem_block = NULL;
	}

	/* Disable Rx Channel */
	i2s_rx_channel_disable(i2s);
	/* Disable Rx Block */
	i2s_rx_block_disable(i2s);

	/* Disable Rx Interrupt */
	i2s_disable_rx_interrupt(i2s);
	/* Disable Master Clock */
	i2s_clock_disable(i2s);
	/* alp-sdk issue #2149 (round 2): the shared clock is now genuinely
	 * off -- invalidate TX's one-shot restart-skip flag so a later TX
	 * restart reprograms instead of skipping onto a clock that just got
	 * gated (round-2 review finding 1). */
	dev_data->tx.clk_restart_skip_ok = false;
}

/*
 * alp-sdk issue #2149: shared body for every TX teardown path that goes
 * through tx_stream_disable()/_ex(). keep_clock is true for exactly one
 * caller -- i2s_tx_irq_handler()'s queue-empty underrun exit -- so a codec
 * relying on this I2S bus for its own bit clock does not see clock loss
 * and latch SHUTDOWN mid-playback. Every other caller of this pair
 * (STOP/DRAIN/DROP via the tx_stream_disable() wrapper below, and the
 * ISR's own already-ERROR and last_block exits) passes/keeps false and
 * gates the clock exactly as before -- including DROP out of ERROR, so an
 * abandoned stream does not leave the clock running forever. PM suspend
 * (i2s_disable_controller(), below CONFIG_PM_DEVICE) never goes through
 * this pair at all -- it clears CER.CLKEN itself, unconditionally, exactly
 * as before this change.
 */
static void tx_stream_disable_ex(struct stream *stream, const struct device *dev, bool keep_clock)
{
	const struct i2s_dw_cfg *i2s = dev->config;

	if (stream->mem_block != NULL) {
		k_mem_slab_free(stream->cfg.mem_slab, stream->mem_block);
		stream->mem_block = NULL;
	}
	/* Disable Tx Channel */
	i2s_tx_channel_disable(i2s);
	/* Disable Tx Block */
	i2s_tx_block_disable(i2s);

	/* Disable Tx Interrupt */
	i2s_disable_tx_interrupt(i2s);

	if (!keep_clock) {
		/* Disable Master Clock */
		i2s_clock_disable(i2s);
		/* alp-sdk issue #2149 (round 2): every teardown that actually
		 * gates the clock here (STOP/DRAIN/DROP via the
		 * tx_stream_disable() wrapper, and the ISR's own
		 * already-ERROR/last_block exits) must also invalidate the
		 * one-shot restart-skip flag -- only the ISR's keep-clock
		 * exit sets it, and it must never survive past a real
		 * clock-off (round-2 review finding 1). */
		stream->clk_restart_skip_ok = false;
	}
}

static void tx_stream_disable(struct stream *stream, const struct device *dev)
{
	tx_stream_disable_ex(stream, dev, false);
}

static void rx_queue_drop(struct stream *stream)
{
	size_t size;
	void *mem_block;

	while (queue_get(&stream->mem_block_queue, &mem_block, &size) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, mem_block);
	}

	k_sem_reset(&stream->sem);
}

static void tx_queue_drop(struct stream *stream)
{
	size_t size;
	void *mem_block;
	unsigned int n = 0U;

	while (queue_get(&stream->mem_block_queue, &mem_block, &size) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, mem_block);
		n++;
	}

	for (; n > 0; n--) {
		k_sem_give(&stream->sem);
	}
}

#if defined(CONFIG_PM_DEVICE)

static void i2s_disable_controller(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;

	/* Disable I2S */
	i2s_global_disable(i2s);

	i2s_clock_disable(i2s);
}

static int i2s_suspend(const struct device *dev)
{
	int ret;
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *data = dev->data;

	data->irq_mask_cache = i2s_get_interrupt_mask(i2s);

	i2s_disable_tx_interrupt(i2s);
	i2s_disable_rx_interrupt(i2s);

	i2s_disable_controller(dev);
	/* alp-sdk issue #2149 (round 2): suspend gates the clock
	 * unconditionally -- invalidate TX's one-shot restart-skip flag so
	 * resume/restart reprograms instead of trusting a stale flag from
	 * before the suspend (round-2 review finding 1). */
	data->tx.clk_restart_skip_ok = false;

	if (i2s->clk_dev != NULL) {
		ret = clock_control_off(i2s->clk_dev, i2s->clkid);
		if (ret != 0 && ret != -EALREADY) {
			LOG_ERR("Unable to turn off clock: err:%d", ret);
			return ret;
		}
	}

#if defined(CONFIG_PINCTRL)
	/* Apply sleep pin configuration if available */
	if (i2s->pincfg != NULL) {
		ret = pinctrl_apply_state(i2s->pincfg, PINCTRL_STATE_SLEEP);
		if (ret < 0 && ret != -ENOENT) {
			/* Ignore -ENOENT (sleep state not defined) */
			return ret;
		}
	}
#endif

	return 0;
}

static int i2s_resume(const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *data = dev->data;
	int ret;

#if defined(CONFIG_PINCTRL)
	if (i2s->pincfg != NULL) {
		ret = pinctrl_apply_state(i2s->pincfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			LOG_ERR("I2S pinctrl setup failed (%d)", ret);
			return ret;
		}
	}
#endif

	if (i2s->clk_dev) {
		/* Reconfigure I2S clock sources */
		ret = clock_control_configure(i2s->clk_dev,
				i2s->clkid, NULL);
		if (ret != 0 && ret != -ENOSYS && ret != -ENOTSUP) {
			LOG_ERR("Unable to configure clock: err:%d", ret);
			return ret;
		}

		ret = clock_control_on(i2s->clk_dev, i2s->clkid);
		if (ret != 0 && ret != -EALREADY) {
			LOG_ERR("Unable to turn on clock: err:%d", ret);
			return ret;
		}
	}

	i2s_enable_controller(dev);
	/* alp-sdk issue #2149 (round 2): resume re-enables CLKEN
	 * unconditionally without reprogramming the divider -- invalidate
	 * TX's one-shot restart-skip flag defensively so the next
	 * tx_stream_start() always reprograms rather than trusting a flag
	 * that predates the suspend (round-2 review finding 1). */
	data->tx.clk_restart_skip_ok = false;

	i2s_set_interrupt_mask(data->irq_mask_cache, i2s);

	return 0;
}

static int i2s_pm_action(const struct device *dev,
			enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		return i2s_suspend(dev);

	case PM_DEVICE_ACTION_RESUME:
		return i2s_resume(dev);

	case PM_DEVICE_ACTION_TURN_OFF:
	case PM_DEVICE_ACTION_TURN_ON:
		/* Power domain handling is automatic via PM framework */
		return 0;

	default:
		break;
	}
	return -ENOTSUP;
}
#endif /* CONFIG_PM_DEVICE */

#define I2S_DW_INIT(index)						\
									\
static void i2s_dw_irq_config_func_##index(const struct device *dev);	\
									\
IF_ENABLED(DT_INST_NODE_HAS_PROP(index, pinctrl_0),			\
	(PINCTRL_DT_INST_DEFINE(index)));				\
									\
static const struct i2s_dw_cfg i2s_dw_config_##index = {		\
	.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(index)),		\
	.clkid = (clock_control_subsys_t) DT_INST_CLOCKS_CELL_BY_IDX(index, 0, clkid),	\
	.cfg.wss_len = WSS_LEN,						\
	.cfg.tx_fifo_trg_lvl = TX_FIFO_TRG_LVL,				\
	.cfg.rx_fifo_trg_lvl = RX_FIFO_TRG_LVL,				\
	.paddr = (struct I2S_Type *)DT_INST_REG_ADDR(index),		\
	.irq_config = i2s_dw_irq_config_func_##index,			\
	IF_ENABLED(DT_INST_NODE_HAS_PROP(index, pinctrl_0),		\
		(.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(index),))	\
};									\
									\
struct queue_item							\
rx_##index##_dw_ring_buf[CONFIG_I2S_DW_RX_BLOCK_COUNT + 1];		\
struct queue_item							\
tx_##index##_dw_ring_buf[CONFIG_I2S_DW_TX_BLOCK_COUNT + 1];		\
static struct i2s_dw_data i2s_dw_data_##index = {			\
	.tx = {.stream_start = tx_stream_start,				\
		.stream_disable = tx_stream_disable,			\
		.queue_drop = tx_queue_drop,				\
		.mem_block_queue.buf = tx_##index##_dw_ring_buf,	\
		.mem_block_queue.len = ARRAY_SIZE(tx_##index##_dw_ring_buf)  },\
	.rx = {.stream_start = rx_stream_start,				\
		.stream_disable = rx_stream_disable,			\
		.queue_drop = rx_queue_drop,				\
		.mem_block_queue.buf = rx_##index##_dw_ring_buf,	\
		.mem_block_queue.len = ARRAY_SIZE(rx_##index##_dw_ring_buf)  },\
};									\
PM_DEVICE_DT_INST_DEFINE(index, i2s_pm_action);				\
DEVICE_DT_INST_DEFINE(index,						\
		      &i2s_dw_initialize, PM_DEVICE_DT_INST_GET(index),	\
		      &i2s_dw_data_##index,				\
		      &i2s_dw_config_##index, POST_KERNEL,		\
		      CONFIG_I2S_INIT_PRIORITY, &i2s_dw_driver_api);	\
									\
static void i2s_dw_irq_config_func_##index(const struct device *dev)	\
{									\
	IRQ_CONNECT(DT_INST_IRQN(index),				\
		    DT_INST_IRQ(index, priority),			\
		    i2s_dw_isr, DEVICE_DT_INST_GET(index), 0);		\
	irq_enable(DT_INST_IRQN(index));				\
}

DT_INST_FOREACH_STATUS_OKAY(I2S_DW_INIT)
