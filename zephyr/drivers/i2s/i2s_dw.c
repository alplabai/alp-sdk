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
 *     (clk_restart_skip_ok, set ONLY by tx_stream_park_keep_clock() -- the
 *     underrun exit above and, since issue #2205, an RX start pre-empting a
 *     RUNNING TX -- and cleared by every path that can invalidate it; see
 *     the field's own comment) confirms CER.CLKEN is already set for the
 *     exact restart the flag was armed for, instead of writing CCR live,
 *     which its own doc comment says must be done with the clock disabled.
 *   - issue #2150 phase 1 (RX clock ownership, this change): the two gaps
 *     the paragraph above used to describe as "NOT fixed here" are now
 *     fixed. rx_stream_disable() (STOP/DRAIN/DROP and both RX-ISR error
 *     exits route through it) no longer gates CER.CLKEN when
 *     tx_clock_is_live() says TX is depending on it -- TX RUNNING, or
 *     tx.clk_restart_skip_ok still set regardless of state (round-3 review
 *     finding 1: a state-only ERROR qualifier on the flag missed
 *     I2S_TRIGGER_PREPARE's ERROR->READY move, which does not clear the
 *     flag -- see tx_clock_is_live()'s own comment), ANDed with the
 *     hardware CER.CLKEN bit itself so a stale software RUNNING left over
 *     from a PM suspend never counts as live. rx_stream_start() no longer
 *     reprograms clock_control_set_rate()/CCR while TX is live at the SAME
 *     frame_clk_freq, and returns -EBUSY instead of silently retuning the
 *     shared divider when TX is live at a DIFFERENT rate. When TX is not
 *     live, both functions behave exactly as before this change. #2150
 *     itself is filed as a full-duplex FEATURE request; this is phase 1
 *     only -- clock-ownership safety, not full duplex.
 *     i2s_dw_configure() still maps I2S_DIR_BOTH to -ENOSYS and
 *     dev_data->dir is still a single field, both deliberately unchanged.
 *     The driver is half-duplex; issue #2205 below made the two
 *     START-time halves of that explicit (a START of one direction
 *     pre-empts a RUNNING stream of the other into I2S_STATE_ERROR, and
 *     dev_data->dir follows START), which closed the first two gaps this
 *     note used to list -- "a start silences the other direction's channel
 *     while its state still says RUNNING" and "once RX is configured a
 *     RUNNING TX is never serviced again". One gap remains, out of scope
 *     for phase 1:
 *       - issue #2179's unserviced-source mask (i2s_dw_isr(), this change)
 *         turns one reachable full-duplex hang into a silent stall instead
 *         of the storm it used to be: alp_i2s_open(RX) -> configure(RX) ->
 *         START(RX) leaves RX RUNNING; the app then calls configure(TX) on
 *         the same device. i2s_dw_configure()'s state check inspects only
 *         the TX stream (tx.state == I2S_STATE_NOT_READY), so it succeeds
 *         and repoints dev_data->dir to I2S_DIR_TX while RX is still
 *         RUNNING. The next RXDA is unserviced, i2s_dw_isr() masks it
 *         (RXDAM), and the RX stream goes deaf while its state still says
 *         RUNNING. The backend reads with SYS_FOREVER_MS, so a blocked
 *         alp_i2s_read() then waits until the TX START pre-empts RX, which
 *         releases it with -EIO (issue #2205); without a TX START it waits
 *         forever. This is strictly an improvement over the pre-fix
 *         behaviour -- that same sequence used to produce the ISR storm
 *         this change kills -- but it is still a gap, not a fix, and stays
 *         with #2150's later phase(s). The mirror image (configure(RX)
 *         while TX is RUNNING) stalls TX the same way until the RX START
 *         pre-empts it, since i2s_dw_configure() still repoints
 *         dev_data->dir too.
 *   - issue #2179 (defensive hardening, this change): an interrupt source
 *     that is asserted and unmasked but whose i2s_dw_isr() branch the
 *     dev_data->dir gate skips used to make that function read ISR, match
 *     no branch, change nothing, and return -- and since the source is
 *     level-held, the NVIC tail-chains straight back in and the core spins
 *     with no fault taken. Four sites now make that unreachable:
 *     i2s_dw_isr() masks any asserted-and-unmasked source it did not
 *     service; rx_stream_start()/tx_stream_start() mask the OTHER
 *     direction's interrupts alongside the channel disable they already
 *     did; i2s_dw_configure() assigns dev_data->dir only after validating
 *     the direction and the stream state, so a failed call no longer
 *     repoints the ISR gate; and i2s_dw_initialize() quiesces
 *     IER/IRER/ITER/RER/TER and masks interrupts BEFORE irq_config() arms
 *     the NVIC, since this block is not in the SYSRESETREQ reset domain
 *     and carries its register state across a warm reset. The cause of
 *     the i2s3 RX-start spin reported in #2179 is PROVEN on silicon: #2179
 *     closed on an A/B capture on e1m-aen-evk-03 in which, with this
 *     change reverted, the first i2s_dw_isr() entry after the RX start saw
 *     ISR & ~IMR = 0x00000010 (TXFE, unmasked, with dir == I2S_DIR_RX)
 *     and the core stormed (3/3); with it, 6/6 ran clean. In the clean arm
 *     rx_stream_start()'s TX mask alone prevented it -- the i2s_dw_isr()
 *     guard never fired and stays as defence in depth. See each site's
 *     own comment.
 *   - issue #2205 (the E8 channel-enable model, this change): on the
 *     Alif E8, TER bit 0 (TXCHENX) and RER bit 0 (RXCHENX) are read-only
 *     -- measured on e1m-aen-evk-03 i2s3 with the block idle, RER and TER
 *     both still read 0x00FFFF01 after the init-time clears (see
 *     I2S_TER_TXCHEN_Msk in i2s_dw.h) -- so i2s_tx_channel_disable() and
 *     i2s_rx_channel_disable() never stopped anything there, and an RX
 *     start left TX running in hardware while no ISR serviced it. The
 *     channel-disable calls stay (they are real on the E7); what changed:
 *       - i2s_dw_isr() masks EVERY unserviced source, not only TXFE/RXDA,
 *         and i2s_dw_initialize() writes the full named IMR mask
 *         (I2S_IMR_ALL_Msk, including the E8's TXFUM) instead of setting
 *         bits 0/1/4/5 and inheriting bit 6 from the previous image;
 *       - rx_stream_start()/tx_stream_start() set dev_data->dir, so a TX
 *         restarted after an RX session is serviced again;
 *       - a RUNNING TX pre-empted by an RX START goes to I2S_STATE_ERROR
 *         through tx_stream_park_keep_clock() -- the same exit #2149's
 *         underrun uses, so the clock is kept, clk_restart_skip_ok is
 *         armed, and the next write() gets -EIO and the backend's
 *         ERROR->PREPARE->START retry restarts it. A RUNNING RX pre-empted
 *         by a TX START goes to I2S_STATE_ERROR through rx_stream_disable(),
 *         which keeps CER.CLKEN whenever tx_clock_is_live(). Either way a
 *         reader or writer already blocked on the pre-empted stream's
 *         semaphore is released with -EIO -- the backend waits with
 *         SYS_FOREVER_MS, and a blocked alp_i2s_read() used to deadlock
 *         the handle's alp_i2s_close();
 *       - i2s_rx_channel_enable()/i2s_tx_channel_enable() read-modify-write,
 *         so the E8's per-slot enables in bits 8-23 survive.
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
#include <zephyr/sys/barrier.h>

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
	size_t             mem_block_size;
	uint32_t  mem_block_offset;
	/* alp-sdk issue #2149 (round 2): one-shot flag. Set ONLY by
	 * tx_stream_park_keep_clock(), the single path that leaves CER.CLKEN
	 * set on purpose -- reached from i2s_tx_irq_handler()'s queue-empty
	 * underrun exit and (issue #2205) from rx_stream_start() pre-empting a
	 * RUNNING TX. tx_stream_start() consults it
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

		ret = clock_control_set_rate(
		    i2s->clk_dev, i2s->clkid, (clock_control_subsys_rate_t)(uintptr_t)sclk);
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

	/* alp-sdk issue #2179: assign dev_data->dir only once this call is
	 * committed to (re)configuring a stream. It used to be the first
	 * statement in this function, so a call that went on to fail -- the
	 * I2S_DIR_BOTH -ENOSYS above, the default -EINVAL, or the invalid-state
	 * -EINVAL just above -- still repointed the ONLY field i2s_dw_isr()
	 * consults to decide which handler to run, permanently, on a call that
	 * changed nothing else. I2S_DIR_BOTH is the worst of the three: it
	 * leaves dir matching NEITHER ISR branch, so from then on every I2S
	 * interrupt is unserviced (see i2s_dw_isr()'s own #2179 comment). A
	 * failed configure() now leaves the gate pointing at whatever direction
	 * was last configured successfully. */
	dev_data->dir = dir;

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
	bool parked;
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
			parked = stream->state == I2S_STATE_ERROR;
			irq_unlock(key);
			/* alp-sdk issue #2205: refused either way, but only a
			 * non-ERROR state is a caller bug worth an error log --
			 * see the DRAIN case below. */
			if (parked) {
				LOG_DBG("STOP trigger: stream in ERROR, use DROP");
			} else {
				LOG_ERR("STOP trigger: invalid state");
			}
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
			parked = stream->state == I2S_STATE_ERROR;
			irq_unlock(key);
			/* alp-sdk issue #2205: DRAIN still needs RUNNING, and a
			 * stream in ERROR still gets -EIO. But ERROR is an expected
			 * state -- the TX underrun exit and a START of the other
			 * direction both park a stream there -- and DROP is the way
			 * out, which is exactly what src/backends/i2s/zephyr_drv.c's
			 * z_stop() falls back to. Logging that refusal at error
			 * level printed the invalid-state error on every clean
			 * teardown after a pre-emption; it is debug-level now.
			 * Any other non-RUNNING state (READY, NOT_READY) is a caller
			 * bug and stays at error level. */
			if (parked) {
				LOG_DBG("DRAIN trigger: stream in ERROR, use DROP");
			} else {
				LOG_ERR("DRAIN trigger: invalid state");
			}
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

	/* alp-sdk issue #2205: the stream can be parked in ERROR while this
	 * call waits on tx.sem -- rx_stream_start() pre-empting a RUNNING TX
	 * gives tx.sem exactly to release such a writer. Queueing into an
	 * ERROR stream would report success for audio PREPARE then drops, so
	 * fail it instead; the backend's ERROR->PREPARE->START retry takes it
	 * from there. No give-back: the count this writer took was the extra
	 * one, and PREPARE/DROP's tx_queue_drop() re-saturates tx.sem. */
	if (dev_data->tx.state == I2S_STATE_ERROR) {
		return -EIO;
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

/*
 * alp-sdk issues #2149 and #2205: stop a TX stream WITHOUT gating the bit
 * clock and park it in I2S_STATE_ERROR. The TX channel/block/interrupt are
 * disabled; CER.CLKEN stays set, so a codec on this bus never sees bit-clock
 * loss and never latches its own SHUTDOWN; clk_restart_skip_ok is armed so
 * the restart does not reprogram a divider that never stopped. The app's
 * next write() gets -EIO, and the backend's ERROR->PREPARE->START retry
 * (src/backends/i2s/zephyr_drv.c) restarts the stream. Two callers:
 *   - i2s_tx_irq_handler()'s queue-empty underrun exit (#2149);
 *   - rx_stream_start() pre-empting a RUNNING TX (#2205). Before this, TX
 *     was left "RUNNING" with nothing draining its queue (dev_data->dir had
 *     moved to I2S_DIR_RX), so every later write() stalled on the
 *     backend's 2-block slab until its timeout and the stream never
 *     recovered.
 */
static void tx_stream_park_keep_clock(struct stream *stream, const struct device *dev)
{
	stream->state = I2S_STATE_ERROR;
	stream->clk_restart_skip_ok = true;
	tx_stream_disable_ex(stream, dev, true);
}

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
	size_t         size   = stream->mem_block_size;
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
				/* alp-sdk issue #2149: this is the underrun exit --
				 * the queue genuinely ran dry mid-playback, not a
				 * caller-requested stop/drain/drop. Park the stream
				 * in ERROR with CER.CLKEN kept set and the one-shot
				 * restart-skip flag armed; every other tx_disable
				 * entry (already-ERROR, last_block, STOPPING) still
				 * gates the clock exactly as before. */
				tx_stream_park_keep_clock(stream, dev);
				return;
			}
			goto tx_disable;
		}
		k_sem_give(&stream->sem);
	}
	return;

tx_disable:
	tx_stream_disable(stream, dev);
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
	/* alp-sdk issue #2179: ISR and IMR share a bit layout (RXDA/RXDAM 0,
	 * RXFO/RXFOM 1, TXFE/TXFEM 4, TXFO/TXFOM 5, and on the E8 TXFU/TXFUM
	 * 6 -- see i2s_dw.h), and IMR is mask-to-DISABLE, so `ISR & ~IMR` is
	 * exactly the set of sources asserted AND unmasked, i.e. the sources actually holding this IRQ
	 * line up. Each branch below clears the bit(s) it services; whatever
	 * is left at the bottom was not serviced and gets masked there. */
	uint32_t unserviced;

	/* Get the Current Interrupt Status*/
	int_status = i2s->paddr->ISR;
	unserviced = int_status & ~i2s_get_interrupt_mask(i2s);

	if ((dev_data->dir == I2S_DIR_TX) &&
	    (_FLD2VAL(I2S_ISR_TXFE, int_status))) {
		/* Handle Tx Interrupt */
		i2s_tx_irq_handler(dev);
		unserviced &= ~I2S_ISR_TXFE_Msk;
	}
	if ((dev_data->dir == I2S_DIR_RX) &&
	    (_FLD2VAL(I2S_ISR_RXDA, int_status))) {
		/* Handle Rx Interrupt */
		i2s_rx_irq_handler(dev);
		unserviced &= ~I2S_ISR_RXDA_Msk;
	}

	/* This should not happen */
	if (_FLD2VAL(I2S_ISR_TXFO, int_status)) {
		i2s_clear_tx_overrun(i2s);
		unserviced &= ~I2S_ISR_TXFO_Msk;
	}

	if (_FLD2VAL(I2S_ISR_RXFO, int_status)) {
		/* Clear overrun interrupt */
		i2s_clear_rx_overrun(i2s);

		/* Disable the Rx Overflow interrupt for now. This will */
		/* be enabled again when Receive function is called */
		i2s_disable_rx_fo_interrupt(i2s);
		unserviced &= ~I2S_ISR_RXFO_Msk;
	}

	/* alp-sdk issue #2179: both data branches above are gated on
	 * dev_data->dir, which this half-duplex driver keeps as a SINGLE field
	 * (I2S_DIR_BOTH is -ENOSYS). A source that is asserted and unmasked but
	 * whose branch the dir gate skipped -- TXFE while dir == I2S_DIR_RX is
	 * the reachable case, since TXFE is asserted by an EMPTY TX FIFO, which
	 * is its resting state, and neither i2s_enable_rx_interrupt() nor the
	 * TX-channel disable in rx_stream_start() used to touch TXFEM -- would
	 * otherwise make this function read ISR, match no branch, change
	 * NOTHING, and return. These sources are level-held, so the NVIC
	 * tail-chains straight back in and the core spins here with no fault
	 * taken. Masking the unserviced source is the one action that is
	 * guaranteed to change the interrupt condition, so this function can no
	 * longer return without having done so. Note that adding a clear inside
	 * i2s_tx_irq_handler()/i2s_rx_irq_handler() would NOT close this: on
	 * this path the handler is never called at all.
	 *
	 * The mask is not lost: i2s_enable_tx_interrupt()/
	 * i2s_enable_rx_interrupt() unmask again on the next
	 * tx_stream_start()/rx_stream_start() for that direction, so a
	 * direction that is later started normally is unaffected. A source that
	 * IS serviced above is never masked here (its bit was cleared from
	 * `unserviced`), so both working single-direction paths behave exactly
	 * as before.
	 *
	 * alp-sdk issue #2205: mask EVERY unserviced source, not only TXFE and
	 * RXDA. The E8's TXFU (bit 6) has no branch above; if it were ever
	 * unmasked, a latched TXFU would be neither serviced, cleared nor
	 * masked -- the same storm shape. Limited to I2S_IMR_ALL_Msk so no 1
	 * is ever written to IMR bits 2-3, which neither SVD defines.
	 *
	 * PROVEN ON SILICON (#2179): the i2s3 RX-start spin measured 6/6 on
	 * e1m-aen-evk-03 (live core in i2s_dw_isr()/_isr_wrapper, CycleCnt
	 * advancing, CFSR = 0x00000000, thread mode starved) is this exact
	 * case. #2179 closed on an A/B capture: with the #2179 change reverted
	 * (this guard AND rx_stream_start()'s TX mask), the first i2s_dw_isr()
	 * entry after the RX start saw ISR & ~IMR = 0x00000010 -- TXFE
	 * asserted and unmasked while dev_data->dir == I2S_DIR_RX -- and
	 * stormed. With the change, rx_stream_start()'s TX mask alone kept
	 * TXFE masked and this guard never fired, so it is the backstop, not
	 * the fix that was exercised. */
	unserviced &= I2S_IMR_ALL_Msk;
	if (unserviced != 0U) {
		i2s_set_interrupt_mask(i2s_get_interrupt_mask(i2s) | unserviced, i2s);
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

	/* alp-sdk issue #2179: quiesce the block and mask every interrupt
	 * BEFORE arming the NVIC, not after. This block is NOT in the
	 * SYSRESETREQ reset domain -- measured on e1m-aen-evk-03: CER and TER
	 * both survived a J-Link `loadbin`'s implicit SYSRESETREQ at
	 * 0x00000001 -- so IER/IMR/IRER/ITER/RER/TER all carry over from
	 * whatever the PREVIOUS image left behind. (The all-zero CER/TER read
	 * straight after a RESETPIN reset was an UNCLOCKED block, not a reset
	 * value: IER read 0x00000000 there too instead of its 0x00000F00
	 * reset value, and once clocked, just after the channel clears below,
	 * RER and TER read their SVD reset value 0x00FFFF01 -- issue #2205.)
	 * The old order called irq_config() (IRQ_CONNECT + irq_enable) with
	 * those registers still holding the previous image's state, and only
	 * masked two lines later; a latched,
	 * unmasked source therefore had a window in which it could be delivered
	 * to an ISR whose dev_data->dir is fresh BSS 0 (== I2S_DIR_RX) and
	 * whose streams are not configured at all. Quiescing first closes the
	 * window instead of racing it.
	 *
	 * CER is deliberately NOT cleared here: i2s_enable_controller() below
	 * sets CER.CLKEN, and "CER.CLKEN reads 1 from i2s_dw_initialize()
	 * onward" is the invariant tx_clock_is_live() and #2149's restart-skip
	 * flag are both built on. It just happens a few instructions later now.
	 *
	 * The k_sem_init() calls also move above irq_config() for the same
	 * reason: i2s_rx_irq_handler()/i2s_tx_irq_handler() both k_sem_give()
	 * these semaphores.
	 *
	 * Inferred, not confirmed on silicon: the 0-byte-console boot in issue
	 * #2179 (CycleCnt advancing, CFSR = 0x00000000, PC in i2s_dw_isr(),
	 * ram_console_buf all zeros) is consistent with a storm resuming inside
	 * this function before the old masking lines ran. #2179's A/B capture
	 * proved the RX-start storm, not this boot-time one, which was never
	 * reproduced. The ordering is worth fixing on its own terms regardless.
	 *
	 * alp-sdk issue #2205: IER/IRER/ITER are what quiesce the block here.
	 * The two channel disables are no-ops on the E8 (TXCHENX/RXCHENX are
	 * read-only -- RER and TER still read 0x00FFFF01 after them, measured
	 * with the block idle) and are kept for the E7, where they are real. */
	i2s_global_disable(i2s);
	i2s_rx_block_disable(i2s);
	i2s_tx_block_disable(i2s);
	i2s_rx_channel_disable(i2s);
	i2s_tx_channel_disable(i2s);

	/* alp-sdk issue #2205: mask every interrupt with ONE full write of the
	 * named mask, not the read-modify-write of bits 0/1/4/5 that
	 * i2s_disable_tx_interrupt()/i2s_disable_rx_interrupt() do. The RMW
	 * kept whatever the previous image left in the other bits, so an
	 * inherited unmask of the E8's TXFUM (bit 6) would have survived into
	 * every boot, and nothing in this driver services TXFU. Nothing in-tree
	 * unmasks bit 6 today; this closes the hole, it fixes no observed
	 * fault. I2S_IMR_ALL_Msk (0x73) is
	 * also the E8's IMR reset value; bits 2-3 are left 0 (no SVD field). */
	i2s_set_interrupt_mask(I2S_IMR_ALL_Msk, i2s);

	k_sem_init(&dev_data->rx.sem, 0, CONFIG_I2S_DW_RX_BLOCK_COUNT);
	k_sem_init(&dev_data->tx.sem, CONFIG_I2S_DW_TX_BLOCK_COUNT,
		   CONFIG_I2S_DW_TX_BLOCK_COUNT);

	/* alp-sdk issue #2179: make the IMR mask writes above architecturally
	 * visible before the NVIC line is armed below -- without a barrier
	 * here, irq_config()'s IRQ_CONNECT()/irq_enable() is free to complete,
	 * and thus become live at the NVIC, before the masking writes have
	 * actually posted. barrier_dsync_fence_full() rather than a bare
	 * __DSB(): this file is also host-compiled on native_sim (CMSIS
	 * intrinsics do not exist there), and Zephyr's portable barrier
	 * degrades to a no-op on any arch without CONFIG_BARRIER_OPERATIONS_*,
	 * which is the correct behaviour on a host with no NVIC to race. */
	barrier_dsync_fence_full();

	i2s->irq_config(dev);

	/* Enable and configure the I2S controller */
	i2s_enable_controller(dev);

	LOG_INF("%s inited", dev->name);

	return 0;
}

/*
 * alp-sdk issue #2150 (phase 1): the bit-clock divider (CCR) and gate
 * (CER.CLKEN) are shared hardware between the RX and TX streams on this
 * controller -- there is one CGU-derived master clock, not one per
 * direction. "TX is live" means TX is actively depending on that clock
 * right now:
 *   - I2S_STATE_RUNNING always qualifies -- TX is actively streaming.
 *   - tx.clk_restart_skip_ok qualifies on its OWN, independent of state.
 *     That flag's own contract (see struct stream) is already
 *     state-independent -- "CER.CLKEN is deliberately left set" -- armed
 *     ONLY by tx_stream_park_keep_clock() (i2s_tx_irq_handler()'s
 *     queue-empty underrun exit, issue #2149, and rx_stream_start()
 *     pre-empting a RUNNING TX, issue #2205) and cleared by every path
 *     that actually gates the clock.
 *     round-3 review finding 1: an earlier ERROR-only form of this check
 *     (`state == ERROR && flag`) was narrower than the flag's own contract
 *     and reopened a hole -- I2S_TRIGGER_PREPARE moves TX ERROR->READY
 *     without clearing the flag, so `state == READY && flag == true &&
 *     CER.CLKEN == 1` was reachable and the ERROR-only check answered
 *     "not live" for it, right up until the app's next write() re-arms the
 *     real START. That gap is not a narrow window: z_start()'s own
 *     PREPARE-retry (src/backends/i2s/zephyr_drv.c) commonly leaves TX
 *     parked in READY exactly like this, for as long as the producer stays
 *     stalled.
 *   - ANDed with i2s_clock_is_enabled(i2s) (the CER.CLKEN hardware read,
 *     same idiom tx_stream_start() already uses) so that software state
 *     alone can never claim TX is live: hardware-only would be wrong the
 *     other way, since CER.CLKEN reads 1 from i2s_dw_initialize() onward
 *     regardless of whether TX has ever streamed, which would wrongly gate
 *     every RX-only test/stream too. Both halves are required. This AND
 *     also closes PM suspend's own gap for free: i2s_suspend() gates
 *     CER.CLKEN unconditionally without touching tx.state, so
 *     tx.state == RUNNING with the clock actually off is reachable -- but
 *     i2s_clock_is_enabled(i2s) reads that same hardware bit, so this
 *     function correctly answers "not live" in that case without needing
 *     its own state-tracking fix (see i2s_suspend()'s own comment).
 * Getting this narrower than it needs to be -- e.g. checking only RUNNING,
 * or re-adding the ERROR-only qualifier above -- reopens the exact
 * silent-amp bug #2149 fixed: an RX overrun or a plain RX stop landing
 * while TX sits parked with the flag armed and the clock still on would
 * gate the clock #2149 deliberately kept alive. NOT_READY and any state
 * with the flag already clear are correctly excluded by the second half
 * above -- in each of those the clock is already gated (or was never on),
 * so RX teardown/start acting on it is a no-op, not a hazard.
 */
static bool tx_clock_is_live(const struct i2s_dw_cfg *i2s, const struct i2s_dw_data *dev_data)
{
	return (dev_data->tx.state == I2S_STATE_RUNNING || dev_data->tx.clk_restart_skip_ok) &&
	       i2s_clock_is_enabled(i2s);
}

static int rx_stream_start(struct stream *stream, const struct device *dev)
{
	const struct i2s_dw_cfg *i2s = dev->config;
	struct i2s_dw_data *const dev_data = dev->data;
	bool tx_live = tx_clock_is_live(i2s, dev_data);
	bool clk_needs_reprogram;
	unsigned int key;
	int ret;

	if (tx_live && dev_data->tx.cfg.frame_clk_freq != stream->cfg.frame_clk_freq) {
		/* alp-sdk issue #2150 (phase 1): one CCR divider is shared by
		 * both directions. TX is live at a DIFFERENT rate than this
		 * RX start is asking for -- retuning it would silently pull
		 * the shared clock out from under the running TX stream, so
		 * refuse instead of pretending to succeed. -EBUSY: the
		 * shared clock resource is contended right now, not
		 * permanently unsupported (-ENOTSUP) and not a bad argument
		 * on its own (-EINVAL) -- retrying once TX stops, or moves
		 * to this same rate, would succeed.
		 */
		return -EBUSY;
	}

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

	/* alp-sdk issue #2205: this driver is half-duplex, so an RX START
	 * pre-empts a RUNNING TX. It used to rely on i2s_tx_channel_disable()
	 * below for that, which is a no-op on the E8 (TXCHENX is read-only),
	 * while the repointed dev_data->dir meant no ISR ever serviced TX
	 * again: TX kept "RUNNING" with nothing draining its queue. Park it in
	 * ERROR instead, through the same keep-clock exit #2149's underrun
	 * uses, so the next write() gets -EIO and the backend's
	 * ERROR->PREPARE->START retry restarts it. The clock is kept and
	 * clk_restart_skip_ok is armed, so tx_clock_is_live() stays true: the
	 * #2150 phase 1 / #2171 contract holds -- a later rx_stream_disable()
	 * leaves CER.CLKEN set -- and tx_live above (read before this) still
	 * skips the reprogram below. Done before RX's interrupts are unmasked
	 * and under irq_lock() together with the dir switch, so no ISR entry
	 * can see RX sources with dir still I2S_DIR_TX, or run TX's
	 * already-ERROR exit (which gates the clock) against this stream.
	 * dev_data->dir follows START, not only configure(): after an RX
	 * session, tx_stream_start() points it back at TX. */
	key = irq_lock();
	if (dev_data->tx.state == I2S_STATE_RUNNING) {
		tx_stream_park_keep_clock(&dev_data->tx, dev);
		/* Release a writer blocked in i2s_dw_write() on a full queue:
		 * with dir now I2S_DIR_RX nothing else would ever give tx.sem,
		 * and the backend configures SYS_FOREVER_MS. The writer sees
		 * ERROR and returns -EIO. ponytail: one give releases one
		 * waiter; the Zephyr I2S API has one writer per stream. */
		k_sem_give(&dev_data->tx.sem);
	}
	dev_data->dir = I2S_DIR_RX;
	irq_unlock(key);

	/* alp-sdk issue #2150 (phase 1): only reprogram the shared bit-clock
	 * divider when TX is not depending on it right now (see
	 * tx_clock_is_live()). When TX IS live, the mismatched-rate case
	 * already returned -EBUSY above, so TX must be live at this SAME
	 * rate -- the clock is already configured and running for it; leave
	 * CCR/CER alone, and leave TX's one-shot restart-skip flag alone
	 * too, since the clock never actually stopped and the flag's
	 * contract (see struct stream) is unaffected by this RX start. When
	 * TX is NOT live, behave exactly as before this fix: invalidate the
	 * flag (round-2 review finding 1: RX reprogramming the same divider
	 * must not let a stale flag skip a later TX restart) and
	 * reprogram. */
	clk_needs_reprogram = !tx_live;
	if (clk_needs_reprogram) {
		dev_data->tx.clk_restart_skip_ok = false;
		/* Configure the I2S Peripheral Clock */
		i2s_configure_clocksource(true, i2s, stream->cfg.frame_clk_freq);
	}

	/* Reset the Rx FIFO */
	i2s_rx_fifo_reset(i2s);
	/* Set WLEN */
	i2s_rx_config_wlen(i2s, stream->cfg.word_size);
	/* Enable Master Clock */
	if (clk_needs_reprogram) {
		i2s_configure_clock(i2s);
	}
	i2s_clock_enable(i2s);
	/* Disable Tx Channel -- alp-sdk issue #2205: a no-op on the E8, real
	 * on the E7; a RUNNING TX was already stopped (ITER) above. */
	i2s_tx_channel_disable(i2s);
	/* alp-sdk issue #2179: mask TX's interrupts alongside the TX channel
	 * disable directly above. TXFE is asserted by an EMPTY TX FIFO -- the
	 * resting state of a TX that is not being fed -- and
	 * i2s_enable_rx_interrupt() below clears only RXDAM|RXFOM, so TXFEM
	 * left unmasked by an earlier TX phase survived into this RX stream.
	 * #2179's A/B capture proved that is the RX-start storm (ISR & ~IMR =
	 * 0x00000010 on the first ISR entry with the #2179 change reverted),
	 * and with it this line alone prevented the storm; i2s_dw_isr()'s
	 * guard never fired and is the backstop. For a TX that was RUNNING,
	 * the park above already did this; the call still covers a TX left
	 * READY or ERROR. */
	i2s_disable_tx_interrupt(i2s);

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
	struct i2s_dw_data *const dev_data = dev->data;
	unsigned int key;
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

	/* alp-sdk issue #2205: the mirror of rx_stream_start()'s pre-emption.
	 * A RUNNING RX goes to I2S_STATE_ERROR through rx_stream_disable() --
	 * the body every RX teardown shares -- instead of relying on
	 * i2s_rx_channel_disable() below, a no-op on the E8. rx_stream_disable()
	 * keeps CER.CLKEN when tx_clock_is_live() (a restart armed by
	 * tx_stream_park_keep_clock()); otherwise it gates the clock and clears
	 * the flag, and the reprogram below turns it back on at TX's rate.
	 * dev_data->dir follows START: an RX session in between no longer
	 * leaves this TX restart unserviced. Placed before the flag is read,
	 * and after queue_get() so a START that fails does not kill RX. */
	key = irq_lock();
	if (dev_data->rx.state == I2S_STATE_RUNNING) {
		dev_data->rx.state = I2S_STATE_ERROR;
		rx_stream_disable(&dev_data->rx, dev);
		/* Release a reader blocked in i2s_dw_read(): with dir now
		 * I2S_DIR_TX nothing else would ever give rx.sem, the backend
		 * configures SYS_FOREVER_MS, and alp_i2s_close() waits for
		 * that read to leave before its DROP could reset the
		 * semaphore -- a deadlock. The reader finds the queue empty
		 * and returns -EIO; PREPARE/DROP's k_sem_reset() clears the
		 * extra count. ponytail: one give releases one waiter. */
		k_sem_give(&dev_data->rx.sem);
	}
	dev_data->dir = I2S_DIR_TX;
	irq_unlock(key);

	clk_needs_reprogram = !stream->clk_restart_skip_ok || !i2s_clock_is_enabled(i2s);
	/* alp-sdk issue #2149 (round 2): the flag is one-shot -- consume
	 * (clear) it on this restart regardless of which branch below
	 * actually runs. Only tx_stream_park_keep_clock() re-arms it. */
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

	/* Disable Rx Channel -- alp-sdk issue #2205: a no-op on the E8, real
	 * on the E7; a RUNNING RX was already stopped (IRER) above. */
	i2s_rx_channel_disable(i2s);
	/* alp-sdk issue #2179: mask RX's interrupts alongside the RX channel
	 * disable directly above, mirroring rx_stream_start(). RXDAM left
	 * unmasked by an earlier RX phase is the quieter half of the same
	 * defect -- RXDA needs data to arrive, and a disabled RX channel
	 * delivers none, so it does not storm the way a stale TXFEM does -- but
	 * leaving a source unmasked that this direction's ISR gate will never
	 * service is the shape of the bug, not the specific bit. */
	i2s_disable_rx_interrupt(i2s);

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
	/* alp-sdk issue #2150 (phase 1): see tx_clock_is_live()'s own
	 * comment for what "TX is live" means. This function is the single
	 * shared body reached from STOP/DRAIN/DROP (via the
	 * stream->stream_disable pointer) AND all four of i2s_rx_irq_handler()'s
	 * `goto rx_disable` routes (round-3 review finding 5: the prior wording
	 * said "both ... error exits", which undercounted and mischaracterised
	 * them) -- the ERROR-state exit, the STOPPING-state exit (not an error:
	 * this is the ISR's own half of a STOP/DRAIN/DROP already in progress),
	 * the failed-k_mem_slab_alloc() exit, and the failed-queue_put() exit --
	 * plus (issue #2205) tx_stream_start() pre-empting a RUNNING RX, so
	 * gating the check here covers every RX teardown path in one place. */
	bool tx_live = tx_clock_is_live(i2s, dev_data);

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

	/* alp-sdk issue #2150 (phase 1): the bit clock is shared with TX.
	 * Gating it here -- from a plain RX stop/drain/drop OR an RX
	 * overrun/error -- while TX is live would pull the same clock TX
	 * depends on out from under it, including the exact keep-clock
	 * window #2149 put TX into on purpose. Only gate when TX is not
	 * live; when TX not live this is unchanged from before this fix. */
	if (!tx_live) {
		/* Disable Master Clock */
		i2s_clock_disable(i2s);
		/* alp-sdk issue #2149 (round 2): the shared clock is now
		 * genuinely off -- invalidate TX's one-shot restart-skip flag
		 * so a later TX restart reprograms instead of skipping onto a
		 * clock that just got gated (round-2 review finding 1). */
		dev_data->tx.clk_restart_skip_ok = false;
	}
	/* alp-sdk issue #2150 (phase 1): when TX IS live, deliberately leave
	 * tx.clk_restart_skip_ok untouched here -- the clock never actually
	 * stopped, so the flag's own contract (see struct stream) is
	 * unaffected by this RX teardown. */
}

/*
 * alp-sdk issue #2149: shared body for every TX teardown path that goes
 * through tx_stream_disable()/_ex(). keep_clock is true for exactly one
 * caller -- tx_stream_park_keep_clock() (the ISR's queue-empty underrun
 * exit, and since issue #2205 an RX start pre-empting a RUNNING TX) -- so a
 * codec relying on this I2S bus for its own bit clock does not see clock
 * loss and latch SHUTDOWN mid-playback. Every other caller of this pair
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

/* alp-sdk issue #2150 (phase 1, round-3 review finding 6): this gates
 * CER.CLKEN unconditionally without touching tx.state, so a system-PM
 * suspend landing mid-stream leaves tx.state == I2S_STATE_RUNNING with the
 * clock actually off -- pm_device_busy_set() (see the TX START trigger)
 * does not block suspend. No separate fix is needed here: tx_clock_is_live()
 * ANDs its state check with i2s_clock_is_enabled(i2s), the same hardware bit
 * this function clears, so it already answers "not live" for exactly this
 * case. */
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
