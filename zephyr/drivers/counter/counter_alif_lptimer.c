/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clean-room Zephyr counter-class driver for the Alif Ensemble LPTIMER
 * (low-power timer), compatible "alif,lptimer".  On the E1M-AEN801 / Ensemble E8
 * SoM this binds the always-on LPTIMER block at lptimer@42001000: four
 * independent 32-bit DOWN-counters (channels 0..3), each clocked from the
 * VBAT-domain low-frequency source and each with its own NVIC line (60..63 on
 * E8) firing on underflow (count reaches zero).
 *
 * It exposes the standard Zephyr counter_* class API
 * (start/stop/get_value/get_top_value/set_alarm/cancel_alarm/get_pending_int) --
 * the same surface the alp-sdk wildcard counter backend
 * (src/backends/counter/zephyr_drv.c) drives via the alp-counter alias.
 *
 * Counter direction.  The Alif LPTIMER is a DOWN-counter (Alif DFP
 * Driver_LPTIMER.c ARM_LPTIMER_GET_COUNT returns the decrementing CURRENTVAL;
 * ARM_LPTIMER_SET_COUNT1 loads 0xFFFFFFFF in free-run mode).  In free-running
 * mode (CONTROLREG MODE bit CLEAR) it loads 0xFFFFFFFF, counts DOWN, and reloads
 * 0xFFFFFFFF on underflow.  So get_value() returns a value that DECREASES over
 * time; this is reported with the counter top value 0xFFFFFFFF and WITHOUT
 * COUNTER_CONFIG_INFO_COUNT_UP.
 *
 * Alarms keep the count (#2944).  One channel has one LOADCOUNT and no compare
 * register, so set_alarm() must reload it.  The driver therefore keeps a
 * software base (counter_alif_lptimer_clock.h): set_alarm()/cancel_alarm() fold
 * the elapsed count into it before reloading, and the alarm ISR folds the
 * period and restores free-run (LOADCOUNT 0xFFFFFFFF), so get_value() is a
 * continuous free-running down-counter and get_top_value() stays 0xFFFFFFFF.
 * Residual error: the clocks between reading CURRENTVAL and the reload taking
 * effect (a few ticks) are lost per set_alarm()/cancel_alarm().  The ISR folds
 * one period plus the clocks since the reload, so ISR latency must stay below
 * one alarm period: at 32768 Hz a 1- or 2-tick alarm (30.5/61 us) is only
 * accurate if the ISR runs within that, else whole periods are lost.
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 -- a thin in-tree Zephyr driver over the memory-mapped
 * LPTIMER registers (the alifsemi/zephyr_alif fork ships NO Zephyr LPTIMER
 * driver and NO "alif,lptimer" binding, and hal_alif exposes no Zephyr device
 * for it; the only upstream code is the proprietary DFP CMSIS driver, which is
 * not consumable).  Distinct always-on block from the LPRTC (counter_dw_rtc.c)
 * and the UTIMER (counter_alif_utimer.c).  BENCH-UNVERIFIED.  INTERIM until E8
 * bench, then revisit.  See docs/adr/0017.
 *
 * CLOCK-GATE NOTE (BENCH-UNVERIFIED).  The LPTIMER input clock is selected (and
 * thereby gated on) per channel via the VBAT-domain TIMER_CLKSEL register
 * (0x1A609004); the driver performs that select in init from the DT
 * `clock-source` property.  Whether the chosen VBAT LF source (32 kHz / 128 kHz)
 * is itself running on the alp-sdk upstream-Zephyr build path is bench-TBD (the
 * always-on VBAT domain typically leaves it on; if CURRENTVAL never advances,
 * the source clock is the cause).  Do NOT add further VBAT writes without the
 * Alif TRM confirming the bits.
 * ====================================================================
 */

#define DT_DRV_COMPAT alif_lptimer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>
#include <stdbool.h>
#include "counter_alif_lptimer.h"
#include "counter_alif_lptimer_clock.h"

LOG_MODULE_REGISTER(counter_alif_lptimer, CONFIG_COUNTER_LOG_LEVEL);

struct counter_alif_lptimer_config {
	struct counter_config_info info;
	void (*irq_config_func)(void);
	uint32_t base;         /* LPTIMER block base (the selected channel block) */
	uint8_t  channel;      /* LPTIMER channel 0..3 */
	uint8_t  clock_source; /* LPTIMER_CLK_SRC_* (VBAT TIMER_CLKSEL field value) */
};

struct counter_alif_lptimer_data {
	counter_alarm_callback_t alarm_cb;
	void                    *user_data;
	bool                     running;
	struct lptimer_clock     clk;
};

/* Per-channel config block: base + channel * stride (Alif DFP soc.h:
 * LPTIMER_CHANNEL_CFG[channel], 0x14 bytes each). */
static inline uint32_t ch_base(const struct counter_alif_lptimer_config *cfg)
{
	return cfg->base + (uint32_t)cfg->channel * LPTIMER_CHANNEL_STRIDE;
}

static uint32_t counter_alif_lptimer_get_pending_int(const struct device *dev);

/* Snapshot CURRENTVAL and "underflow pending" consistently: pending, cur,
 * pending again; if the pending bit changed in between, read cur again.
 * Caller holds irq_lock. */
static uint32_t lptimer_snapshot(const struct device *dev, bool *underflowed)
{
	const struct counter_alif_lptimer_config *cfg  = dev->config;
	const struct counter_alif_lptimer_data   *data = dev->data;
	bool                                      p1, p2;
	uint32_t                                  cur;

	p1  = counter_alif_lptimer_get_pending_int(dev) != 0U;
	cur = sys_read32(ch_base(cfg) + LPTIMER_CH_CURRENTVAL);
	p2  = counter_alif_lptimer_get_pending_int(dev) != 0U;
	if (p1 != p2) {
		cur = sys_read32(ch_base(cfg) + LPTIMER_CH_CURRENTVAL);
	}
	/* A pending underflow only counts while an alarm is armed. */
	*underflowed = p2 && data->alarm_cb != NULL;
	return cur;
}

static int counter_alif_lptimer_start(const struct device *dev)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	uint32_t                                  ctrl;
	unsigned int                              key;

	if (data->running) {
		return -EALREADY;
	}

	/*
	 * Free-running mode: MODE bit CLEAR (Alif DFP lptimer.h
	 * set_mode_freerunning clears LPTIMER_CTRL_MODE), load 0xFFFFFFFF
	 * (Alif DFP lptimer.h load_max_count), then set ENABLE.
	 *
	 * ENABLE must be CLEARED first.  HWRM 13.1.4.4, the NOTE under Figure
	 * 13-5: "Before programming the timer mode or the load count value, the
	 * timer must be disabled in order to avoid potential synchronization
	 * issues.  This ensures that any information on these signals is always
	 * communicated to the timer while it is inactive and they are stable
	 * whenever the timer is enabled."
	 *
	 * This is reachable, not theoretical: cancel_alarm() used to leave the
	 * hardware enabled in user-defined mode while clearing data->running, so
	 * set_alarm() -> cancel_alarm() -> start() sailed past the -EALREADY guard
	 * above and wrote MODE and LOADCOUNT into a live timer across an
	 * asynchronous clock-domain boundary (#1829).  set_alarm() below already
	 * did this correctly; start() now mirrors it.
	 */
	key = irq_lock();

	ctrl = sys_read32(cbase + LPTIMER_CH_CONTROLREG);
	ctrl &= ~(LPTIMER_CTRL_ENABLE | LPTIMER_CTRL_MODE);
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	sys_write32(LPTIMER_LOAD_MAX, cbase + LPTIMER_CH_LOADCOUNT);
	lptimer_clock_reset(&data->clk);

	ctrl |= LPTIMER_CTRL_ENABLE;
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	irq_unlock(key);

	data->running = true;
	LOG_DBG("%p LPTIMER ch%u started (free-run)", dev, cfg->channel);
	return 0;
}

static int counter_alif_lptimer_stop(const struct device *dev)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	uint32_t                                  ctrl;
	unsigned int                              key = irq_lock();

	/* Clear ENABLE (Alif DFP lptimer.h disable_counter clears
	 * LPTIMER_CTRL_ENABLE); drop any pending alarm so a later set_alarm()
	 * is not -EBUSY. */
	ctrl = sys_read32(cbase + LPTIMER_CH_CONTROLREG);
	ctrl &= ~(LPTIMER_CTRL_ENABLE | LPTIMER_CTRL_MODE);
	ctrl |= LPTIMER_CTRL_INT_MASK;
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	data->alarm_cb  = NULL;
	data->user_data = NULL;
	data->running   = false;
	irq_unlock(key);
	return 0;
}

static int counter_alif_lptimer_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct counter_alif_lptimer_data *data = dev->data;

	/* CURRENTVAL is the live (decrementing) count (Alif DFP lptimer.h
	 * get_count reads CURRENTVAL).  HWRM 13.1.4.5: "A 0 is always read back
	 * when the timer is not enabled." -- so a caller that stops the counter and
	 * reads it used to get a plausible-looking 0 with a success return.  Say
	 * -EIO instead of handing back a number that means nothing (#1829).
	 *
	 * (The manual contradicts itself on the idle value: 13.1.5.3.2 lists the
	 * CURRENTVALUE reset value as 0x8000_0000.  Either way it is not a count.) */
	if (!data->running) {
		return -EIO;
	}

	unsigned int key = irq_lock();
	bool         underflowed;
	/* Underflow while an alarm is armed but the ISR has not folded it yet is
	 * counted by lptimer_clock_value(). */
	uint32_t cur = lptimer_snapshot(dev, &underflowed);

	*ticks = lptimer_clock_value(&data->clk, cur, underflowed);
	irq_unlock(key);
	return 0;
}

/*
 * Required Zephyr counter-class op: counter_get_top_value() calls this with NO
 * NULL guard, so it MUST be present.
 *
 * Free-running, the reload value is 0xFFFFFFFF (Alif DFP lptimer.h
 * load_max_count).  set_alarm() reprograms LOADCOUNT (the wrap point in
 * user-defined mode, HWRM 13.1.4.6) only until the alarm fires; get_value()
 * is the software-continued free-running count, so the top stays UINT32_MAX.
 */
static uint32_t counter_alif_lptimer_get_top_value(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* The virtual counter always wraps at 2^32 (alarms are bookkept in
	 * software, #2944), whatever LOADCOUNT currently holds. */
	return UINT32_MAX;
}

/* Reload `load` with MODE/INT_MASK as given (ENABLE is cleared first, HWRM
 * 13.1.4.4, then set again), then wait (bounded, ~100 us = 3 LF clocks) until
 * CURRENTVAL shows the new segment: it keeps the previous count, or 0 before the
 * first LF edge after ENABLE, and a stale read would be folded into the base
 * as real elapsed time (#2944).  Caller holds irq_lock. */
static void lptimer_reload(uint32_t cbase, const struct lptimer_clock *clk, bool user_mode)
{
	uint32_t load = clk->load;
	uint32_t t0;
	uint32_t bound = (sys_clock_hw_cycles_per_sec() / 1000000U) * 100U;

	uint32_t ctrl = sys_read32(cbase + LPTIMER_CH_CONTROLREG);

	ctrl &= ~LPTIMER_CTRL_ENABLE;
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	if (user_mode) {
		ctrl |= LPTIMER_CTRL_MODE;
		ctrl &= ~LPTIMER_CTRL_INT_MASK;
	} else {
		ctrl &= ~LPTIMER_CTRL_MODE;
		ctrl |= LPTIMER_CTRL_INT_MASK;
	}
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	sys_write32(load, cbase + LPTIMER_CH_LOADCOUNT);

	ctrl |= LPTIMER_CTRL_ENABLE;
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	t0 = k_cycle_get_32();
	while (!lptimer_clock_start_seen(clk, sys_read32(cbase + LPTIMER_CH_CURRENTVAL)) &&
	       (k_cycle_get_32() - t0) < bound) {
	}
}

static int counter_alif_lptimer_set_alarm(const struct device            *dev,
                                          uint8_t                         chan_id,
                                          const struct counter_alarm_cfg *alarm_cfg)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	unsigned int                              key;

	/* One alarm channel per LPTIMER counter (the underflow IRQ). */
	if (chan_id != 0) {
		LOG_ERR("invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	/* The LPTIMER underflow fires after a fixed number of down-counts, so
	 * only a RELATIVE alarm maps cleanly onto it.  An ABSOLUTE alarm would
	 * need the counter to pass through a target value, which the
	 * reload-on-underflow free-running model does not signal. */
	if (alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		LOG_ERR("absolute alarm not supported on LPTIMER");
		return -ENOTSUP;
	}

	/*
	 * HWRM 13.1.4.7: "When PWM is disabled (TIMER_PWM bit = 0x0), the low and
	 * high periods of the timer toggle output are the same and are equal to:
	 * (LPTIMERn_LOADCOUNT + 1) x LPTIMERn_CLK clock period."  Table 13-5 fixes
	 * the toggle output as changing state on every reload and 13.1.4.3 puts the
	 * interrupt on that same reload, so the alarm interval is LOADCOUNT + 1
	 * clocks -- and the driver used to program LOADCOUNT = ticks.
	 *
	 * At clock-frequency = 32768 with ticks = 1 that fired at 2/32768 s =
	 * 61.0 us instead of 30.5 us, a 100% error.  The relative error is
	 * 1/ticks, so it hid at long delays and was grossly wrong at short ones
	 * (#1829).  ticks == 0 has no representation at all.
	 */
	if (alarm_cfg->ticks == 0U) {
		LOG_ERR("alarm ticks must be >= 1 (LOADCOUNT + 1 clocks per HWRM 13.1.4.7)");
		return -EINVAL;
	}

	/* Fold the count so far into the software base, then reload the channel
	 * in user-defined mode (MODE bit SET -> reload LOADCOUNT on underflow,
	 * Alif DFP lptimer.h set_mode_userdefined) with ticks - 1 so underflow
	 * fires after `ticks` clocks.  The read-then-reload window loses a few
	 * ticks (see counter_alif_lptimer_clock.h).  set_alarm() on a stopped
	 * counter starts it. */
	key = irq_lock();

	/* Pending alarm: -EBUSY per the Zephyr counter API (checked under the
	 * lock so two threads cannot both pass). */
	if (data->alarm_cb != NULL) {
		irq_unlock(key);
		return -EBUSY;
	}

	if (data->running) {
		lptimer_clock_fold(&data->clk,
		                   sys_read32(cbase + LPTIMER_CH_CURRENTVAL),
		                   false,
		                   alarm_cfg->ticks - 1U);
	} else {
		lptimer_clock_reset(&data->clk);
		data->clk.load = alarm_cfg->ticks - 1U;
	}
	lptimer_reload(cbase, &data->clk, true);

	data->alarm_cb  = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;
	data->running   = true;

	irq_unlock(key);

	LOG_DBG("%p LPTIMER ch%u alarm in %u ticks", dev, cfg->channel, alarm_cfg->ticks);
	return 0;
}

static int counter_alif_lptimer_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	unsigned int                              key;

	if (chan_id != 0) {
		LOG_ERR("invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	/*
	 * Fold the elapsed count and restore free-run (LOADCOUNT 0xFFFFFFFF, MODE
	 * clear, interrupt masked) so the counter keeps counting and get_value()
	 * stays continuous (#2944).  Masking alone would leave the timer cycling
	 * in the alarm period (HWRM 13.1.4.6).  If the alarm already underflowed
	 * but the ISR has not run, count the full period too.  Nothing armed:
	 * nothing to do, the counter is not stopped.
	 *
	 * Under irq_lock because the ISR touches the same state and registers;
	 * HWRM 13.1.1 NOTE also requires serialised access to the LP peripherals.
	 */
	key = irq_lock();

	if (data->alarm_cb != NULL) {
		bool     underflowed;
		uint32_t cur = lptimer_snapshot(dev, &underflowed);

		lptimer_clock_fold(&data->clk, cur, underflowed, LPTIMER_CLK_FREE_LOAD);
		lptimer_reload(cbase, &data->clk, false);
		data->alarm_cb  = NULL;
		data->user_data = NULL;
	}

	irq_unlock(key);
	return 0;
}

static uint32_t counter_alif_lptimer_get_pending_int(const struct device *dev)
{
	const struct counter_alif_lptimer_config *cfg = dev->config;

	/* LPTIMERS_INTSTATUS is a per-channel pending bitmap (Alif DFP soc.h
	 * LPTIMERS_INTSTATUS); bit `channel` set => this channel's underflow IRQ
	 * is pending. */
	uint32_t status = sys_read32(cfg->base + LPTIMER_INTSTATUS);

	return (status & BIT(cfg->channel)) ? 1U : 0U;
}

static void counter_alif_lptimer_isr(const struct device *dev)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	counter_alarm_callback_t                  cb    = NULL;
	void                                     *user_data = NULL;
	uint32_t                                  ticks = 0U;
	unsigned int                              key   = irq_lock();

	/* Fold only if the underflow is really pending: a latched NVIC pending
	 * left over from a cancelled/stopped alarm must not count a phantom
	 * period against a freshly armed one. */
	if (data->alarm_cb != NULL && counter_alif_lptimer_get_pending_int(dev) != 0U) {
		cb        = data->alarm_cb;
		user_data = data->user_data;

		/* Fold the full period (plus the ISR latency, i.e. the clocks
		 * since the reload) and restore free-run before the callback, so
		 * the count continues and the callback may re-arm (#2944). */
		lptimer_clock_fold(&data->clk,
		                   sys_read32(cbase + LPTIMER_CH_CURRENTVAL),
		                   true,
		                   LPTIMER_CLK_FREE_LOAD);
		lptimer_reload(cbase, &data->clk, false);
		data->alarm_cb  = NULL;
		data->user_data = NULL;
		/* The free-run segment has just started: no CURRENTVAL read (it
		 * can still show the previous count). */
		ticks = lptimer_clock_value(&data->clk, LPTIMER_CLK_FREE_LOAD, false);
	}

	/* Clear the pending channel interrupt: reading the per-channel EOI
	 * register clears it (Alif DFP lptimer.h clear_interrupt reads
	 * LPTIMER_EOI). */
	(void)sys_read32(cbase + LPTIMER_CH_EOI);
	irq_unlock(key);

	if (cb != NULL) {
		cb(dev, 0, ticks, user_data);
	}
}

static DEVICE_API(counter, counter_alif_lptimer_api) = {
	.start           = counter_alif_lptimer_start,
	.stop            = counter_alif_lptimer_stop,
	.get_value       = counter_alif_lptimer_get_value,
	.get_top_value   = counter_alif_lptimer_get_top_value,
	.set_alarm       = counter_alif_lptimer_set_alarm,
	.cancel_alarm    = counter_alif_lptimer_cancel_alarm,
	.get_pending_int = counter_alif_lptimer_get_pending_int,
};

static int counter_alif_lptimer_init(const struct device *dev)
{
	const struct counter_alif_lptimer_config *cfg   = dev->config;
	struct counter_alif_lptimer_data         *data  = dev->data;
	uint32_t                                  cbase = ch_base(cfg);
	uint32_t                                  clksel;
	uint32_t                                  ctrl;

	data->alarm_cb  = NULL;
	data->user_data = NULL;
	data->running   = false;

	/* Channels 0 and 2 do NOT support the cascade source (Alif DFP
	 * Driver_LPTIMER.c ARM_LPTIMER_Initialize). */
	if ((cfg->channel == 0U || cfg->channel == 2U) &&
	    cfg->clock_source == LPTIMER_CLK_SRC_CASCADE) {
		LOG_ERR("LPTIMER ch%u does not support cascade clock", cfg->channel);
		return -EINVAL;
	}

	/* Select this channel's input clock in the VBAT TIMER_CLKSEL register:
	 * a 2-bit field at bit (channel << 2) (Alif DFP sys_ctrl_lptimer.h
	 * select_lptimer_clk: clear then set channel field). */
	clksel = sys_read32(LPTIMER_VBAT_TIMER_CLKSEL);
	clksel &= ~(LPTIMER_CLKSEL_FIELD_MSK << LPTIMER_CLKSEL_FIELD_POS(cfg->channel));
	clksel |= ((uint32_t)cfg->clock_source << LPTIMER_CLKSEL_FIELD_POS(cfg->channel));
	sys_write32(clksel, LPTIMER_VBAT_TIMER_CLKSEL);

	/* Start disabled, free-running mode, interrupt masked (no alarm armed).
	 * MODE bit CLEAR = free-run (Alif DFP lptimer.h set_mode_freerunning);
	 * MASK bit SET = interrupt masked (Alif DFP lptimer.h mask_interrupt). */
	ctrl = sys_read32(cbase + LPTIMER_CH_CONTROLREG);
	ctrl &= ~(LPTIMER_CTRL_ENABLE | LPTIMER_CTRL_MODE);
	ctrl |= LPTIMER_CTRL_INT_MASK;
	sys_write32(ctrl, cbase + LPTIMER_CH_CONTROLREG);

	cfg->irq_config_func();

	LOG_DBG(
	    "Alif LPTIMER ch%u init (base 0x%08x, clk-src %u)", cfg->channel, cbase, cfg->clock_source);
	return 0;
}

#define COUNTER_ALIF_LPTIMER_INIT(inst)                                                            \
	static void counter_alif_lptimer_irq_config_##inst(void);                                      \
                                                                                                   \
	static struct counter_alif_lptimer_data counter_alif_lptimer_data_##inst;                      \
                                                                                                   \
	static const struct counter_alif_lptimer_config				\
		counter_alif_lptimer_config_##inst = {					\
		.info = {								\
			.max_top_value = UINT32_MAX,					\
			.freq = DT_INST_PROP(inst, clock_frequency),			\
			.flags = 0,	/* DOWN-counter: not COUNT_UP */		\
			.channels = 1,							\
		},									\
		.irq_config_func = counter_alif_lptimer_irq_config_##inst,		\
		.base = DT_INST_REG_ADDR(inst),						\
		.channel = DT_INST_PROP(inst, channel),				\
		.clock_source = DT_INST_PROP(inst, clock_source),			\
	};                                           \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst,                                                                    \
	                      counter_alif_lptimer_init,                                               \
	                      NULL,                                                                    \
	                      &counter_alif_lptimer_data_##inst,                                       \
	                      &counter_alif_lptimer_config_##inst,                                     \
	                      POST_KERNEL,                                                             \
	                      CONFIG_COUNTER_INIT_PRIORITY,                                            \
	                      &counter_alif_lptimer_api);                                              \
                                                                                                   \
	static void counter_alif_lptimer_irq_config_##inst(void)                                       \
	{                                                                                              \
		IRQ_CONNECT(DT_INST_IRQN(inst),                                                            \
		            DT_INST_IRQ(inst, priority),                                                   \
		            counter_alif_lptimer_isr,                                                      \
		            DEVICE_DT_INST_GET(inst),                                                      \
		            0);                                                                            \
		irq_enable(DT_INST_IRQN(inst));                                                            \
	}

DT_INST_FOREACH_STATUS_OKAY(COUNTER_ALIF_LPTIMER_INIT)
