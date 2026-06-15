/*
 * Copyright (C) 2024 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr counter-class driver for the Alif Ensemble UTIMER block.  On the
 * E1M-X / Ensemble E8 SoM this backs the portable alp_counter_* surface: the
 * wildcard counter backend (src/backends/counter/zephyr_drv.c) resolves the
 * `alp-counter0` DT alias to a UTIMER instance and drives it through the
 * standard Zephyr counter_* class API.  Nothing alp-side changes; this just
 * gives that backend a real device to talk to on E8.
 *
 * ============================== STATUS ==============================
 * vendor-ext, BENCH-UNVERIFIED.
 *
 * Upstream Zephyr v4.4 ships NO Alif counter/timer driver, and the Apache-2.0
 * alifsemi/zephyr_alif fork ships only the DTS bindings + nodes for the UTIMER
 * (its drivers/ tree has ethernet/i2c/gpio/spi only -- no counter source).
 * hal_alif (modules/hal/alif) ships ONLY the register-helper library
 * (drivers/utimer/include/utimer.h, alif_utimer_*).  So this driver is newly
 * authored against that helper API; it has NOT been run on real silicon.
 * ====================================================================
 *
 * Node shape (matches the fork + the sibling alp-sdk PWM driver):
 *   - The compatible "alif,utimer-counter" sits on a CHILD node of the
 *     alif,utimer PARENT.  This driver binds the CHILD and reaches the parent
 *     via DT_INST_PARENT() for the two reg windows + timer-id + clock-frequency
 *     + the COMPARE-A interrupt -- exactly as pwm_alif_utimer.c binds the
 *     "alif,pwm" child.  Binding the child (not the parent) keeps the counter
 *     and PWM drivers from colliding on the shared "alif,utimer" compatible.
 *   - Two reg windows on the parent: a per-timer block ("timer", reg[0], e.g.
 *     0x48001000) holding CNTR / COMPARE / INTERRUPT regs, and a shared global
 *     block ("global", reg[1], 0x48000000) where per-bit (by timer-id) counter
 *     START / STOP / CLK_ENABLE live.
 *   - The counter is a free-running 32-bit up-counter (reload = 0xFFFFFFFF);
 *     COMPARE-A drives the single Zephyr alarm channel (channel 0), whose match
 *     raises CHAN_INTERRUPT_COMPARE_A_BUF1 on the parent's "comp_capt_a" line.
 *
 * Clocking note: the fork's ALIF_UTIMER_CLK is a frequency-only dummy with no
 * gate register -- the Zephyr clock controller can neither gate nor report the
 * UTIMER tick rate.  This driver therefore (a) gates the per-timer clock itself
 * via alif_utimer_enable_timer_clock(), and (b) takes the tick rate from the
 * parent node's `clock-frequency` property (counter_get_freq() source for
 * counter_us_to_ticks()).  The board/overlay supplies that rate; the value MUST
 * be confirmed against the Alif Ensemble TRM at bench bring-up.
 *
 * Header dependency: only <utimer.h> (hal_alif, on the include path via
 * modules/hal/alif/drivers/utimer/CMakeLists.txt's zephyr_include_directories)
 * plus standard Zephyr headers.  No fork-only / soc_common.h dependency.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <zephyr/dt-bindings/timer/alif_utimer.h>

#include <utimer.h>

#define DT_DRV_COMPAT alif_utimer_counter

LOG_MODULE_REGISTER(counter_alif_utimer, CONFIG_COUNTER_LOG_LEVEL);

/* COMPARE-A driver index passed to the hal_alif compare helpers. */
#define UTIMER_DRIVER_A 0U

/* Free-running 32-bit reload: the counter wraps at 0xFFFFFFFF. */
#define UTIMER_TOP_VALUE 0xFFFFFFFFU

/*
 * UTIMER counter tick rate placeholder, used only when the parent node omits
 * `clock-frequency`.  The verified value is a silicon-determined HW fact from
 * the Alif Ensemble E8 TRM (the UTIMER clock-tree source) -- NOT invented here
 * (per the alp-sdk pending-hw-configs policy).  A wrong rate only mis-scales
 * counter_us_to_ticks(); it does not change the register programming.
 */
#define UTIMER_DEFAULT_CLK_HZ 100000000U

/*
 * NOTE: counter_config_info MUST be the first member -- the Zephyr counter
 * subsystem casts dev->config directly to (const struct counter_config_info *)
 * for counter_get_top_value()/counter_is_counting_up()/etc.  This is why the
 * driver does NOT use the DEVICE_MMIO machinery (whose DEVICE_MMIO_GET reads
 * offset 0 of dev->config on no-MMU targets like the M55 RTSS, which would
 * collide with `info`).  Instead the hal_alif UTIMER API takes raw 32-bit
 * register bases and the UTIMER window is identity-mapped within the 32-bit
 * address space on the M55 cores, so the physical bases are stored directly --
 * mirroring how the sibling PWM driver carries its `global_base` as a plain
 * uintptr_t.
 */
struct counter_alif_utimer_config {
	struct counter_config_info info;          /* MUST be first (counter API) */
	uint32_t  timer_base;                      /* parent reg[0] ("timer")    */
	uint32_t  global_base;                     /* parent reg[1] ("global")   */
	uint32_t  freq_hz;                         /* tick rate (DTS supplied)   */
	uint8_t   timer_id;                        /* instance id in global block*/
	uint8_t   direction;                       /* ALIF_UTIMER_COUNTER_DIR_*  */
	void (*irq_cfg)(const struct device *dev);
};

struct counter_alif_utimer_data {
	counter_alarm_callback_t alarm_cb;         /* channel-0 alarm callback    */
	void                    *alarm_user;
	uint32_t                 guard_period;
};

static inline uint32_t timer_base(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;

	return cfg->timer_base;
}

static inline uint32_t global_base(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;

	return cfg->global_base;
}

static int counter_alif_utimer_start(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;

	alif_utimer_start_counter(global_base(dev), cfg->timer_id);
	alif_utimer_enable_counter(timer_base(dev));
	return 0;
}

static int counter_alif_utimer_stop(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;

	alif_utimer_stop_counter(global_base(dev), cfg->timer_id);
	alif_utimer_disable_counter(timer_base(dev));
	return 0;
}

static int counter_alif_utimer_get_value(const struct device *dev, uint32_t *ticks)
{
	*ticks = alif_utimer_get_counter_value(timer_base(dev));
	return 0;
}

static uint32_t counter_alif_utimer_get_top_value(const struct device *dev)
{
	/* Free-running 32-bit: reload register holds 0xFFFFFFFF. */
	return alif_utimer_get_counter_reload_value(timer_base(dev));
}

static uint32_t counter_alif_utimer_get_pending_int(const struct device *dev)
{
	return (alif_utimer_get_pending_interrupt(timer_base(dev)) &
		CHAN_INTERRUPT_COMPARE_A_BUF1) ? 1U : 0U;
}

static int counter_alif_utimer_set_alarm(const struct device *dev, uint8_t chan_id,
					 const struct counter_alarm_cfg *acfg)
{
	struct counter_alif_utimer_data *data = dev->data;
	uint32_t base = timer_base(dev);
	uint32_t target;

	/* Single COMPARE-A channel only on first bring-up. */
	if (chan_id != 0U) {
		return -ENOTSUP;
	}

	/* Match counter_set_channel_alarm() semantics: one outstanding alarm. */
	if (data->alarm_cb != NULL) {
		return -EBUSY;
	}

	if (acfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		target = acfg->ticks;
	} else {
		/* Relative: add to the live counter value (free-running, wraps). */
		target = alif_utimer_get_counter_value(base) + acfg->ticks;
	}

	data->alarm_cb   = acfg->callback;
	data->alarm_user = acfg->user_data;

	alif_utimer_set_compare_value(base, UTIMER_DRIVER_A, target);
	alif_utimer_enable_compare_match(base, UTIMER_DRIVER_A);
	alif_utimer_enable_interrupt(base, CHAN_INTERRUPT_COMPARE_A_BUF1_BIT);

	return 0;
}

static int counter_alif_utimer_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	struct counter_alif_utimer_data *data = dev->data;
	uint32_t base = timer_base(dev);

	if (chan_id != 0U) {
		return -ENOTSUP;
	}

	alif_utimer_disable_interrupt(base, CHAN_INTERRUPT_COMPARE_A_BUF1_BIT);
	alif_utimer_disable_compare_match(base, UTIMER_DRIVER_A);
	data->alarm_cb   = NULL;
	data->alarm_user = NULL;

	return 0;
}

static int counter_alif_utimer_set_top_value(const struct device *dev,
					     const struct counter_top_cfg *top_cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(top_cfg);

	/*
	 * Free-running 32-bit only on first bring-up.  The ALP us_to_ticks /
	 * get_value path does not need a programmable top; return -ENOTSUP so
	 * counter_set_top_value() reports the limitation cleanly.
	 */
	return -ENOTSUP;
}

static uint32_t counter_alif_utimer_get_freq(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;

	/* Source for counter_us_to_ticks(); parent's clock-frequency property. */
	return cfg->freq_hz;
}

static uint32_t counter_alif_utimer_get_guard_period(const struct device *dev, uint32_t flags)
{
	struct counter_alif_utimer_data *data = dev->data;

	ARG_UNUSED(flags);
	return data->guard_period;
}

static int counter_alif_utimer_set_guard_period(const struct device *dev, uint32_t ticks,
						uint32_t flags)
{
	struct counter_alif_utimer_data *data = dev->data;

	ARG_UNUSED(flags);
	if (ticks > counter_alif_utimer_get_top_value(dev)) {
		return -EINVAL;
	}
	data->guard_period = ticks;
	return 0;
}

static void counter_alif_utimer_isr(const struct device *dev)
{
	struct counter_alif_utimer_data *data = dev->data;
	uint32_t base = timer_base(dev);
	uint32_t pending = alif_utimer_get_pending_interrupt(base);

	if (pending & CHAN_INTERRUPT_COMPARE_A_BUF1) {
		counter_alarm_callback_t cb;
		void *user;
		uint32_t now;

		alif_utimer_clear_interrupt(base, CHAN_INTERRUPT_COMPARE_A_BUF1_BIT);
		alif_utimer_disable_interrupt(base, CHAN_INTERRUPT_COMPARE_A_BUF1_BIT);
		alif_utimer_disable_compare_match(base, UTIMER_DRIVER_A);

		/* Latch + clear the one-shot alarm before invoking the callback,
		 * so a re-arm from inside the callback is honoured. */
		cb   = data->alarm_cb;
		user = data->alarm_user;
		data->alarm_cb   = NULL;
		data->alarm_user = NULL;

		now = alif_utimer_get_counter_value(base);
		if (cb != NULL) {
			cb(dev, 0, now, user);
		}
	}
}

static int counter_alif_utimer_init(const struct device *dev)
{
	const struct counter_alif_utimer_config *cfg = dev->config;
	uint32_t base = timer_base(dev);

	/* Gate the per-timer clock in the global block (the Zephyr clock
	 * controller cannot -- ALIF_UTIMER_CLK is a frequency-only dummy). */
	alif_utimer_enable_timer_clock(global_base(dev), cfg->timer_id);

	/* Counting direction.  Default UP is the free-running tick source. */
	switch (cfg->direction) {
	case ALIF_UTIMER_COUNTER_DIRECTION_DOWN:
		alif_utimer_set_down_counter(base);
		break;
	case ALIF_UTIMER_COUNTER_DIRECTION_TRIANGLE:
		alif_utimer_set_triangular_counter(base);
		break;
	case ALIF_UTIMER_COUNTER_DIRECTION_UP:
	default:
		alif_utimer_set_up_counter(base);
		break;
	}

	/* Free-running 32-bit: reload at max, start at 0, leave disabled. */
	alif_utimer_set_counter_reload_value(base, UTIMER_TOP_VALUE);
	alif_utimer_set_counter_value(base, 0U);

	/* Wire the COMPARE-A match line to the alarm ISR. */
	cfg->irq_cfg(dev);

	return 0;
}

static DEVICE_API(counter, counter_alif_utimer_api) = {
	.start             = counter_alif_utimer_start,
	.stop              = counter_alif_utimer_stop,
	.get_value         = counter_alif_utimer_get_value,
	.set_alarm         = counter_alif_utimer_set_alarm,
	.cancel_alarm      = counter_alif_utimer_cancel_alarm,
	.set_top_value     = counter_alif_utimer_set_top_value,
	.get_pending_int   = counter_alif_utimer_get_pending_int,
	.get_top_value     = counter_alif_utimer_get_top_value,
	.get_guard_period  = counter_alif_utimer_get_guard_period,
	.set_guard_period  = counter_alif_utimer_set_guard_period,
	.get_freq          = counter_alif_utimer_get_freq,
};

/*
 * The compatible "alif,utimer-counter" sits on the CHILD node; the two reg
 * windows + timer-id + clock-frequency + the COMPARE-A interrupt live on the
 * PARENT alif,utimer node.  Parent reg[0] is the per-timer block, reg[1] the
 * global block; the IRQ is the parent's "comp_capt_a" line.
 */
#define COUNTER_ALIF_UTIMER_INIT(inst)                                                      \
	static void counter_alif_utimer_irq_cfg_##inst(const struct device *dev);           \
	static struct counter_alif_utimer_data counter_alif_utimer_data_##inst;             \
	static const struct counter_alif_utimer_config counter_alif_utimer_cfg_##inst = {   \
		.info = {                                                                   \
			.max_top_value = UTIMER_TOP_VALUE,                                  \
			.freq          = DT_PROP_OR(DT_INST_PARENT(inst), clock_frequency,  \
						    UTIMER_DEFAULT_CLK_HZ),                 \
			.flags         = COUNTER_CONFIG_INFO_COUNT_UP,                      \
			.channels      = 1,                                                 \
		},                                                                          \
		.timer_base  = DT_REG_ADDR_BY_IDX(DT_INST_PARENT(inst), 0),                 \
		.global_base = DT_REG_ADDR_BY_IDX(DT_INST_PARENT(inst), 1),                 \
		.freq_hz     = DT_PROP_OR(DT_INST_PARENT(inst), clock_frequency,            \
					  UTIMER_DEFAULT_CLK_HZ),                           \
		.timer_id    = DT_PROP_OR(DT_INST_PARENT(inst), timer_id, 0),               \
		.direction   = DT_PROP_OR(DT_INST_PARENT(inst), counter_direction,          \
					  ALIF_UTIMER_COUNTER_DIRECTION_UP),                \
		.irq_cfg     = counter_alif_utimer_irq_cfg_##inst,                          \
	};                                                                                  \
	DEVICE_DT_INST_DEFINE(inst, counter_alif_utimer_init, NULL,                         \
			      &counter_alif_utimer_data_##inst,                             \
			      &counter_alif_utimer_cfg_##inst, POST_KERNEL,                 \
			      CONFIG_COUNTER_INIT_PRIORITY, &counter_alif_utimer_api);      \
	static void counter_alif_utimer_irq_cfg_##inst(const struct device *dev)            \
	{                                                                                   \
		ARG_UNUSED(dev);                                                            \
		/* comp_capt_a carries the COMPARE-A match used by the alarm path. */      \
		IRQ_CONNECT(DT_IRQ_BY_NAME(DT_INST_PARENT(inst), comp_capt_a, irq),         \
			    DT_IRQ_BY_NAME(DT_INST_PARENT(inst), comp_capt_a, priority),    \
			    counter_alif_utimer_isr, DEVICE_DT_INST_GET(inst), 0);          \
		irq_enable(DT_IRQ_BY_NAME(DT_INST_PARENT(inst), comp_capt_a, irq));         \
	}

DT_INST_FOREACH_STATUS_OKAY(COUNTER_ALIF_UTIMER_INIT)
