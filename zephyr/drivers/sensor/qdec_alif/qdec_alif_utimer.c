/*
 * Copyright (c) 2025 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored VERBATIM fork-driver copy, INTERIM) ======
 * The Alif Ensemble UTIMER quadrature-decoder is driven by a vendored copy of
 * the Apache-2.0 Alif qdec sensor driver (drivers/sensor/qdec_alif/
 * qdec_alif_utimer.c, compatible "alif,utimer-qdec").  Upstream Zephyr v4.4
 * ships no Alif qdec driver and hal_alif exposes no Zephyr device for the
 * quadrature block -- only the register-helper library (drivers/utimer/include/
 * utimer.h, alif_utimer_*) this file calls -- so the qdec source is carried
 * in-tree as a VENDORED FORK-DRIVER COPY WITH LOCAL FIXES, not a verbatim
 * vendor file.  It started as a verbatim copy; #1828 open-coded the filter
 * programming, added the CNTR_TRIG write and a BUILD_ASSERT bound, and #2037
 * added the missing GLB_CNTR_START write.  Anything that repoints this node
 * onto the opt-in sdk-alif fork compatible MUST carry those four forward or
 * silently reintroduce the defects; retire onto the fork only once the node is
 * repointed AND bench-verified.
 * See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 *
 * Node shape (matches the sibling counter/PWM utimer drivers): the
 * "alif,utimer-qdec" compatible sits on a CHILD node of the "alif,utimer"
 * PARENT.  This driver binds the CHILD and reaches the parent via
 * DT_INST_PARENT() for the two reg windows ("global" 0x48000000 + "timer",
 * the per-channel block, e.g. 0x4800d000 for QEC0 = UTIMER channel 12), the
 * timer-id, and the clock phandle.  Position is read by POLLING the counter
 * value (sensor_sample_fetch); the driver registers no ISR.  The reported
 * SENSOR_CHAN_ROTATION value is scaled to DEGREES
 * (counter * 360 / counts-per-revolution), not raw counts.
 * vendor-ext.  BENCH-VERIFIED for the counter-start path on E1M-AEN803 serial
 * 2026W36-0002 (2026-09-08): after init, CNTR_CTRL (0x4800D080) reads
 * 0x00000023 -- EN | RUNNING | CNTR_TRIG -- and GLB_CNTR_RUNNING (0x4800000C)
 * reads 0x00001000, bit 12 for QEC0's channel.  Both were clear before #2037.
 * STILL UNVERIFIED, and do not read the above as covering it: whether the
 * counter is advancing on real quadrature edges.  In that same run, with
 * nobody touching the shaft, the count advanced steadily -- which no
 * stationary encoder should do.  The RATE is NOT known: the app polls a
 * mod-96 counter every ~300 ms, so the observable is only
 * (rate * 0.3) mod 96, and a whole family of rates share that residue.  Do
 * not quote a counts/s figure from that run; several 32.768 kHz-derived
 * rates fit it as well as any other.  The source is INTERNAL to the channel,
 * not the pads: a later run walked four pad configurations -- no bias
 * (0x00210005), pull-up (0x00290005), pull-up + Schmitt (0x002B0005), and
 * AF=0 with P3_0/P3_1 deselected from the QEC entirely (0x00290000) -- and
 * the counter kept advancing through all four.  Open lead for whoever picks
 * this up: UP_1_SRC/DOWN_1_SRC hold the correct x4 quadrature masks
 * (0x69/0x96) with PGM_EN (bit 31) CLEAR, while START_1_SRC / STOP_1_SRC /
 * CLEAR_1_SRC all have it set; and FILTER_CTRL_B (0x4800D088) is 0x00000000
 * because this driver writes only FILTER_CTRL_A, so the B input is unfiltered
 * while A is filtered.
 */

#define DT_DRV_COMPAT alif_utimer_qdec

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/sys_io.h>

#include "utimer.h"

/*
 * UTIMERn_CNTR_CTRL bit 5 CNTR_TRIG, attributed to HWRM 13.2.6.3.26: "Set this
 * bit if incrementing or decrementing the counter via triggers."  hal_alif's
 * utimer.h defines CNTR_CTRL bits 0, 1, 2, 4, 8 but not this one, and exposes
 * no setter for it (#1828).
 *
 * CONFIRMED against the AE822 SVD, which is in this tree: peripheral UTIMER,
 * register UTIMER_CNTR_CTRL (addressOffset 0x80), field CNTR_TRIG at bit 5 --
 * "Set this bit if incrementing or decrementing the counter via triggers",
 * the exact wording attributed to the HWRM above.  (hal_alif's utimer.h and
 * the Alif DFP header both omit bit 5, and Alif's own QEC reference flow
 * never sets it, which is why this looked unsourced for a while.)
 */
#define QDEC_CNTR_CTRL_TRIG_BIT 5U

LOG_MODULE_REGISTER(qdec_alif_utimer, CONFIG_SENSOR_LOG_LEVEL);

/* QDEC constant configuration parameters */
struct qdec_alif_utimer_config {
	DEVICE_MMIO_NAMED_ROM(global);
	DEVICE_MMIO_NAMED_ROM(timer);
	const uint8_t timer_id;
	bool filter_enable;
	uint8_t filter_prescaler;
	uint8_t filter_taps;
	const struct pinctrl_dev_config *pcfg;
	uint32_t counts_per_revolution;
	const struct device *clk_dev;
	clock_control_subsys_t clkid;
};

/* QDEC run time data */
struct qdec_alif_utimer_data {
	DEVICE_MMIO_NAMED_RAM(global);
	DEVICE_MMIO_NAMED_RAM(timer);
	int32_t position;
};

#define DEV_CFG(_dev) ((const struct qdec_alif_utimer_config *)(_dev)->config)
#define DEV_DATA(_dev) ((struct qdec_alif_utimer_data *const)(_dev)->data)

static int qdec_alif_utimer_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct qdec_alif_utimer_config *cfg = DEV_CFG(dev);
	struct qdec_alif_utimer_data *data = DEV_DATA(dev);
	uintptr_t timer_base = DEVICE_MMIO_NAMED_GET(dev, timer);
	uint32_t counter_value;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_ROTATION)) {
		return -ENOTSUP;
	}

	counter_value = alif_utimer_get_counter_value(timer_base);

	/* 64-bit intermediate: counter_value * 360 overflows a uint32_t once
	 * counts_per_revolution exceeds 11930464, and the DT value is only checked
	 * against < 1 at init -- which on an unsigned type catches nothing but
	 * zero.  A typo of counts-per-revolution = <20000000> produced a wrapped,
	 * arbitrary angle rather than any kind of failure (#1828). */
	data->position = (uint32_t)(((uint64_t)counter_value * 360ULL) / cfg->counts_per_revolution);

	return 0;
}

static int qdec_alif_utimer_channel_get(const struct device *dev, enum sensor_channel chan,
			struct sensor_value *val)
{
	struct qdec_alif_utimer_data *data = DEV_DATA(dev);

	if (chan != SENSOR_CHAN_ROTATION) {
		return -ENOTSUP;
	}

	val->val1 = data->position;
	val->val2 = 0;
	return 0;
}

static const struct sensor_driver_api qdec_alif_utimer_driver_api = {
	.sample_fetch = qdec_alif_utimer_sample_fetch,
	.channel_get = qdec_alif_utimer_channel_get
};

static int qdec_alif_utimer_init(const struct device *dev)
{
	const struct qdec_alif_utimer_config *cfg = DEV_CFG(dev);
	uintptr_t timer_base = DEVICE_MMIO_NAMED_GET(dev, timer);
	uintptr_t global_base = DEVICE_MMIO_NAMED_GET(dev, global);
	int32_t ret;

	/* apply pin configuration */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	if (cfg->counts_per_revolution < 1) {
		LOG_ERR("Invalid number of counts per revolution should be positive");
		return -EINVAL;
	}

	/* check device availability */
	if (!device_is_ready(cfg->clk_dev)) {
		LOG_ERR("clock controller device not ready");
		return -ENODEV;
	}
	/* Enable clock only for lputimer instances from clock manager */
	ret = clock_control_on(cfg->clk_dev, cfg->clkid);
	if (ret != 0) {
		LOG_ERR("Unable to turn on clock: err:%d", ret);
		return ret;
	}

	alif_utimer_enable_timer_clock(global_base, cfg->timer_id);

	/*
	 * ENABLE the software counter control, do not disable it.  HWRM 13.2.6.3.8
	 * defines START_1_SRC[31] PGM_EN as "0x0: Global programmatic start is
	 * disabled": with it cleared, the global START write below is ignored.  The
	 * old alif_utimer_disable_soft_counter_ctrl() here cleared it (#1828).  This
	 * only ARMS the programmatic start/stop/clear sources (the helper sets
	 * CNTR_SRC1_PGM_EN on START_1_SRC, STOP_1_SRC and CLEAR_1_SRC) -- it does
	 * not start anything; the
	 * GLB_CNTR_START write that does is at the end of this function (#2037).
	 */
	alif_utimer_enable_soft_counter_ctrl(timer_base);
	alif_utimer_set_up_counter(timer_base);
	alif_utimer_set_counter_value(timer_base, 0x0);
	alif_utimer_set_counter_reload_value(timer_base, cfg->counts_per_revolution - 1);
	alif_utimer_enable_counter(timer_base);

	if (cfg->filter_enable) {
		/*
		 * Open-coded instead of alif_utimer_enable_filter(), which never
		 * applies its own shift constants:
		 *
		 *     reg |= (prescaler | taps | CHAN_FILTER_CTRL_FILTER_EN);
		 *
		 * HWRM 13.2.6.3.27 UTIMERn_FILTER_CTRL_A: "21-16 PRESCALER, 15-12
		 * RESERVED, 11-8 FILTER_TAPS, 7-1 RESERVED, 0 FILTER_EN".  With
		 * prescaler 4 and taps 3 the HAL wrote 0x7 -- two RESERVED bits plus
		 * FILTER_EN -- leaving both real fields at 0, so the noise filter the
		 * board author configured ran with zero taps and zero prescaler.  Its
		 * clear mask was wrong the same way (#1828).
		 */
		uint32_t filt = sys_read32(UTIMER_FILTER_CTRL_A(timer_base));

		filt &= ~((uint32_t)CHAN_FILTER_CTRL_FILTER_PRESCALER_Msk
		          << CHAN_FILTER_CTRL_FILTER_PRESCALER_BIT);
		filt &= ~((uint32_t)CHAN_FILTER_CTRL_FILTER_TAPS_Msk << CHAN_FILTER_CTRL_FILTER_TAPS_BIT);
		filt |= ((uint32_t)cfg->filter_prescaler & CHAN_FILTER_CTRL_FILTER_PRESCALER_Msk)
		        << CHAN_FILTER_CTRL_FILTER_PRESCALER_BIT;
		filt |= ((uint32_t)cfg->filter_taps & CHAN_FILTER_CTRL_FILTER_TAPS_Msk)
		        << CHAN_FILTER_CTRL_FILTER_TAPS_BIT;
		filt |= CHAN_FILTER_CTRL_FILTER_EN;

		sys_write32(filt, UTIMER_FILTER_CTRL_A(timer_base));
	}

	alif_utimer_config_qdec_triggers(timer_base);

	/*
	 * Put the channel in trigger-based counting.  HWRM 13.2.6.3.26, and
	 * confirmed by the AE822 SVD -- see the QDEC_CNTR_CTRL_TRIG_BIT comment --
	 * UTIMERn_CNTR_CTRL bit 5 CNTR_TRIG: "Set this bit if incrementing or
	 * decrementing the counter via triggers.  0x0: Not in trigger based
	 * increment/decrement mode.  0x1: Trigger based increment/decrement mode."
	 * The quadrature edge triggers are programmed just above via UP_1_SRC /
	 * DOWN_1_SRC, but nothing set this bit and the HAL exposes no way to, so
	 * the channel sat in non-trigger mode while triggers were its only count
	 * source: rotating the encoder left the counter at 0 and sample_fetch()
	 * still returned success (#1828).
	 */
	sys_set_bit(UTIMER_CNTR_CTRL(timer_base), QDEC_CNTR_CTRL_TRIG_BIT);

	/*
	 * Start the channel -- LAST, once every register above is programmed, since
	 * this is the write that lets the hardware act on them.  Enabling and
	 * starting are two separate steps: alif_utimer_enable_counter() above sets
	 * the per-channel UTIMERn_CNTR_CTRL bit 0 EN, which only permits the channel
	 * to run, while HWRM 13.2.5 names the GLOBAL START/STOP/CLEAR writes as how
	 * a channel is actually turned on -- hence the GLOBAL base and the timer id
	 * here, not timer_base (same calling convention as
	 * alif_utimer_enable_timer_clock() above).  Without it the channel sat
	 * configured and idle: measured on E1M-AEN803 2026W36-0002, CNTR_CTRL
	 * (0x4800d080) read 0x00000021 -- EN and CNTR_TRIG set but bit 1 RUNNING
	 * CLEAR -- with GLB_CNTR_START (0x48000000) never written and the counter
	 * stuck at 0 while sample_fetch() still returned success (#2037).  CNTR_CTRL
	 * bit 1 RUNNING (and GLB_CNTR_RUNNING bit <timer_id>) is what proves this
	 * took effect on silicon.
	 */
	alif_utimer_start_counter(global_base, cfg->timer_id);

	return 0;
}

#define CHECK_FILTER_PARAM_VALUES(n) \
	/* <=, not <: HWRM 13.2.6.3.27 gives PRESCALER the range 0x0-0x3F and    \
	 * FILTER_TAPS four bits, so the mask value IS legal -- the assert used  \
	 * to reject exactly 63 while its message said "exceeds maximum of 63"   \
	 * (#1828). */ \
	BUILD_ASSERT((DT_INST_PROP(n, filter_prescaler) <= CHAN_FILTER_CTRL_FILTER_PRESCALER_Msk), \
	             "UTIMER QDEC filter prescaler value exceeds maximum of " STRINGIFY( \
	                 CHAN_FILTER_CTRL_FILTER_PRESCALER_Msk)); \
	BUILD_ASSERT((DT_INST_PROP(n, filter_taps) <= CHAN_FILTER_CTRL_FILTER_TAPS_Msk), \
	             "UTIMER QDEC filter taps value exceeds maximum of " STRINGIFY( \
	                 CHAN_FILTER_CTRL_FILTER_TAPS_Msk));

#define QDEC_ALIF_UTIMER_INIT(n)								\
	PINCTRL_DT_INST_DEFINE(n);								\
	COND_CODE_1(DT_INST_PROP(n, input_filter_enable), (CHECK_FILTER_PARAM_VALUES(n)), ());	\
	static struct qdec_alif_utimer_data qdec_alif_utimer_data_##n;				\
	static const struct qdec_alif_utimer_config qdec_alif_utimer_cfg_##n = {		\
		DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(global, DT_INST_PARENT(n)),			\
		DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(timer, DT_INST_PARENT(n)),			\
		.timer_id = DT_PROP(DT_INST_PARENT(n), timer_id),				\
		.counts_per_revolution = DT_INST_PROP(n, counts_per_revolution),		\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),					\
		.clk_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),			\
		.clkid = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PARENT(n), clkid),	\
		COND_CODE_1(DT_INST_PROP(n, input_filter_enable),				\
		(.filter_enable = DT_INST_PROP(n, input_filter_enable),				\
		.filter_prescaler = DT_INST_PROP(n, filter_prescaler),				\
		.filter_taps = DT_INST_PROP(n, filter_taps)), ())				\
	};											\
												\
	SENSOR_DEVICE_DT_INST_DEFINE(n,								\
				     qdec_alif_utimer_init,					\
				     NULL,							\
				     &qdec_alif_utimer_data_##n,				\
				     &qdec_alif_utimer_cfg_##n,					\
				     POST_KERNEL,						\
				     CONFIG_SENSOR_INIT_PRIORITY,				\
				     &qdec_alif_utimer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(QDEC_ALIF_UTIMER_INIT)
