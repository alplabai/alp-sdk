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
 * added the FILTER_CTRL_B write that makes the two quadrature inputs filter
 * alike and established that a trigger-counting channel must NEVER be started
 * (see the DO-NOT-START block at the end of qdec_alif_initialize()).  #2037
 * (continued) also replaced the QEC-channel (timer_id >= 12) trigger-source
 * programming: the upstream copy called alif_utimer_config_qdec_triggers(),
 * which arms the SRC_1 "channel input A/B" matrix -- correct for the
 * lputimer0/1/2 instances Alif's own tree binds this driver to, but not a
 * QEC channel's input path at all (Alif's own CMSIS driver refuses SRC_1 on
 * a QEC_MODE_ENABLE channel, Driver_UTIMER.c:612-618).  QEC channels now
 * program SRC_0 (UTIMER_UP_0_SRC / UTIMER_DOWN_0_SRC) instead, matching
 * Alif's own qec0_app() (Boards/Templates/Baremetal/demo_qec.c:218-228).
 * Anything that repoints this node onto the opt-in sdk-alif fork compatible
 * MUST carry those forward or silently reintroduce the defects; retire onto
 * the fork only once the node is repointed AND bench-verified.
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
 * the counter kept advancing through all four.  That measurement stands on
 * its own and is unaffected by everything below.
 *
 * BENCH CRITERION for "the spurious count is gone", because nobody will be
 * turning the shaft: read CNTR (0x4800D0A0) RAW over SWD at three
 * well-separated intervals; all three must be BIT-IDENTICAL.  Any change is a
 * FAIL.  Do not substitute the app's printed degrees -- it polls a mod-96
 * counter, so it cannot tell "static" from "advanced by exactly 96*k".  What
 * this criterion can NEVER show is that the counter tracks real quadrature
 * edges; that needs a hand on the shaft (one detent = +-4 raw counts, one
 * revolution CW = +96 back to the start, then CCW).
 *
 * RETRACTED (was an open lead here until #2037): "UP_1_SRC/DOWN_1_SRC hold
 * the x4 masks with PGM_EN (bit 31) CLEAR while START/STOP/CLEAR_1_SRC have
 * it set."  There is no bit 31 in those registers to be clear.  AE822 SVD,
 * peripheral UTIMER: UTIMER_UP_1_SRC (addressOffset 0x1C) and
 * UTIMER_DOWN_1_SRC (0x24) define eight fields in bits [7:0] and nothing
 * else; PGM_EN [31:31] is a field of UTIMER_START_1_SRC (0x04),
 * UTIMER_STOP_1_SRC and UTIMER_CLEAR_1_SRC ONLY -- it gates the PROGRAMMATIC
 * start/stop/clear, and there is no programmatic up/down to gate.  For the
 * same reason UP_0_SRC (0x18) and DOWN_0_SRC (0x20) reading 0x00000000 is
 * not a PGM_EN defect.  Whether it is a defect at all is OPEN (#2038), and an
 * earlier version of this comment got the reason wrong -- read the retraction
 * below before relying on it.
 *
 * RETRACTED, and the misreading is worth stating so it is not repeated: this
 * comment used to claim "the SVD annotates UP_0_SRC with eight 'For QEC
 * channels: Reserved, not used' notes and UP_1_SRC with none, so SRC_1 is the
 * path intended for QEC channels".  Those eight annotations sit on bits
 * [31:24] ONLY (TRIG12..TRIG15).  The other 24 fields, bits [23:0], read "For
 * QEC channels: Rising/Falling edge of QEC_TRIGGER0..11 causes counter to
 * increment" -- so SRC_0 IS a QEC-channel input path, and reading the top
 * eight bits as if they governed the whole register inverted the conclusion.
 *
 * RESOLVED (#2037): "channel input A/B" on a QEC channel (12-15) is NOT the
 * encoder's X/Y pads.  UP_1_SRC 0x69 / DOWN_1_SRC 0x96 formed a complete,
 * disjoint x4 matrix over (A,B) and were measured to never count regardless
 * -- an attended bench run with the raw pads confirmed toggling live while
 * CNTR stayed 0x00000000 the entire time.  Alif bind SRC_1 ("channel input
 * A/B") to the lputimer0/1/2 instances in their own tree; their QEC channels
 * (12-15) count through SRC_0 with QEC_TRIGGER0/1/2 instead, confirmed both
 * by the AE822 SVD (UTIMER_UP_0_SRC/UTIMER_DOWN_0_SRC bits [23:0]: "For QEC
 * channels: Rising/Falling edge of QEC_TRIGGER0..11 causes counter to
 * increment") and by Alif's own CMSIS driver, which REFUSES SRC_1 on a
 * QEC_MODE_ENABLE channel (Driver_UTIMER.c:612-618,
 * ARM_DRIVER_ERROR_PARAMETER unless triggerSrc == ARM_UTIMER_SRC_0).  This
 * driver now programs SRC_0 for timer_id >= 12, matching Alif's own
 * qec0_app() (demo_qec.c:218-228: TRIG0_RISING up, TRIG1_RISING down) --
 * see the SRC_0 write in qdec_alif_utimer_init().  That explains why
 * deselecting P3_0/P3_1 (AF=0) changed nothing about the earlier spurious
 * count under the counter-start bug (#2038): SRC_1 was armed and SRC_0 was
 * not, so neither path was the encoder's real route into the counter.
 *
 * SUPERSEDED (#2037, 2026-09-13, 120000 unaliased CNTR reads over 25 s of
 * continuous hand motion): the SRC_0 fix above IS live -- the stuck-at-zero
 * symptom this whole issue opened on is resolved, CNTR moves where it
 * never did before -- but the channel counts QEC_TRIGGER0/1 edges WITHOUT
 * qualifying the other trigger input, so it is an edge counter, not a
 * quadrature decoder: net +654 over the window (up +2385, down -1731),
 * unwrapped range -33..+710, 293 of 734 non-zero steps |step| >= 2 inside a
 * single 0.21 ms sample -- contact bounce, not motion; a real quadrature
 * pair must net 0 per revolution under this mapping at any speed. Confirmed
 * against the register map (AE822 SVD UTIMER_UP_0_SRC/UTIMER_DOWN_0_SRC
 * bits [23:0] describe ONLY "edge causes counter to increment/decrement",
 * no level qualification of the other input anywhere) and against Alif's
 * own demo_qec.c:306,317,328, which drives X/Y/Z as three independent GPIOs
 * and counts their edges -- an edge-count test, not a quadrature test. So
 * counts-per-revolution (96 in the example overlay) is NOT bench-confirmed
 * for this hardware channel and there is no reload value that would make
 * it one -- see the board overlay's MEASURED paragraph for the full
 * evidence, and #2037's changelog fragment for the software decoder added
 * in response (Zephyr's gpio-qdec input driver, over the same pads, which
 * DOES qualify both phases).
 *
 * GAP, recorded not resolved: whether FILTER_CTRL_A/FILTER_CTRL_B (0x84/
 * 0x88) do anything to the QEC_TRIGGER0..2 inputs on these channels at all
 * is UNPROVEN -- see the filter-write comment lower in this file and the
 * board overlay for the two candidate explanations this leaves open.
 *
 * MOOT for the stuck-at-zero question, possibly still relevant to the
 * decode question: CNTR_TYPE.  Measured CNTR_CTRL 0x00000021 decodes (SVD
 * UTIMER_CNTR_CTRL, 0x80) as CNTR_EN[0]=1, CNTR_RUNNING[1]=0,
 * CNTR_TYPE[4:2]=0 = Sawtooth, CNTR_TRIG[5]=1, CNTR_DIR[8]=0 = Up.  Alif's
 * own QEC flow configures TRIANGLE instead (demo_qec.c qec0_app() passes
 * ARM_UTIMER_COUNTER_TRIANGLE).  Not chased further: the measured defect is
 * a missing level-qualification in the TRIGGER SOURCE registers themselves
 * (SRC_0's bit descriptions never mention the other input at all, at any
 * CNTR_TYPE), so a counter-waveform-shape change would not add the
 * qualification SRC_0 simply does not have.
 *
 * DEAD END, documented so nobody spends a bench slot on it: the "channel
 * drives its own input" theory.  SVD UTIMER_GLB_DRIVER_OEN (0x10) defines
 * DRIVER_OEN_0..DRIVER_OEN_11 across bits [23:0] and nothing above -- QEC
 * channels 12-15 have NO driver outputs, so there is no output to loop back,
 * and Alif's utimer_glb_driver_output_disable() on channel 12 writes nothing.
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

/*
 * SRC_0 trigger-source bit values for QEC channels (timer_id 12-15).
 * hal_alif's utimer.h (drivers/utimer/include/utimer.h:22,24) defines the
 * UTIMER_UP_0_SRC/UTIMER_DOWN_0_SRC ADDRESS macros this driver already uses
 * for the filter registers' neighbours, but no bit-VALUE constants for
 * SRC_0 -- hal_alif's alif_utimer_config_qdec_triggers() only ever
 * programs SRC_1.  These two values are open-coded here the same way the
 * FILTER_CTRL fields above are, and are not invented: they match the Alif
 * DFP's own CNTR_SRC0_TRIG0_RISING (0x00000001) / CNTR_SRC0_TRIG1_RISING
 * (0x00000004) (alif-dfp-ref/drivers/include/utimer.h:34,36).  AE822 SVD,
 * peripheral UTIMER, UTIMER_UP_0_SRC/UTIMER_DOWN_0_SRC (offsets 0x18/0x20),
 * bits [23:0]: "For QEC channels: Rising/Falling edge of QEC_TRIGGER0..11
 * causes counter to increment" -- bit 0 is QEC_TRIGGER0 rising, bit 2 is
 * QEC_TRIGGER1 rising.  The up/down assignment (TRIG0 up, TRIG1 down)
 * matches Alif's own qec0_app() (Boards/Templates/Baremetal/demo_qec.c:
 * 218-224: upcount_trig uses ARM_UTIMER_SRC0_TRIG0_RISING, downcount_trig
 * uses ARM_UTIMER_SRC0_TRIG1_RISING), whose Driver_UTIMER.c REFUSES any
 * other triggerSrc on a QEC_MODE_ENABLE channel (:612-618,
 * ARM_DRIVER_ERROR_PARAMETER unless triggerSrc == ARM_UTIMER_SRC_0).
 */
#define QDEC_SRC0_TRIG0_RISING 0x00000001U
#define QDEC_SRC0_TRIG1_RISING 0x00000004U

/*
 * Bench-triage alternative, OFF by default.  Programs SRC_0 to count BOTH
 * edges of QEC_TRIGGER0 only (TRIG0_RISING | TRIG0_FALLING = 0x00000003)
 * and leaves DOWN_0_SRC at 0x00000000, instead of the up/down split above.
 *
 * This is a LIVENESS check of SRC_0, nothing more, and was already answered
 * once (#2037, 2026-09-13): it counts. It is unsigned by construction --
 * DOWN_0_SRC unarmed means CNTR can only ever increase -- so at x1 edge
 * decode (both edges of one trigger only) it reads 48 counts per mechanical
 * revolution of this 24-PPR part, NOT direction-decoded, and inflated by
 * contact bounce the same way the default (up/down) mapping is measured to
 * be: see the MEASURED paragraph in the board overlay's file header for the
 * 120000-read bounce measurement, which used the default mapping but
 * applies here too -- neither mapping qualifies the other trigger input, so
 * neither can reject a bounced edge. Do not read a run of this build as
 * evidence of a decode; its only question is "does SRC_0 count at all",
 * which is settled. Not a Kconfig/DT knob on purpose -- this is a
 * diagnostic build flag for one bench session, not a shipped configuration.
 */
#ifdef QDEC_ALIF_UTIMER_SRC0_X_EDGES
#define QDEC_SRC0_UP_VALUE   0x00000003U
#define QDEC_SRC0_DOWN_VALUE 0x00000000U
#else
#define QDEC_SRC0_UP_VALUE   QDEC_SRC0_TRIG0_RISING
#define QDEC_SRC0_DOWN_VALUE QDEC_SRC0_TRIG1_RISING
#endif

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
	 * not start anything, and nothing later in this function does either --
	 * see the DO-NOT-START block at the end, which is a measured invariant this
	 * driver depends on, not an omission (#2037).
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

		/*
		 * BOTH inputs, identically -- SCOPED TO SRC_1 / lputimer0/1/2
		 * (timer_id < 12): on THOSE channels the quadrature decode is
		 * level-qualified ACROSS the pair, so filtering one phase and not
		 * the other skews them relative to each other.  AE822 SVD, peripheral
		 * UTIMER: UTIMER_FILTER_CTRL_A (addressOffset 0x84) "Allows the input
		 * A to be sampled periodically", UTIMER_FILTER_CTRL_B (0x88) the same
		 * for input B; UTIMER_UP_1_SRC (0x1C) bit 0 DRIVE_A_RISING_B_0 is
		 * "channel input A is rising AND channel input B = 0 causes counter
		 * to increment", and its seven siblings are qualified the same way.
		 * Delaying A by the filter's sample depth while B arrives raw
		 * therefore lets a transition be sampled against the OTHER phase's
		 * stale level and be classified into the wrong direction -- exactly
		 * how contact bounce on a mechanical encoder nets counts.  This
		 * driver wrote only FILTER_CTRL_A, so B sat at its 0x00000000 reset
		 * (unfiltered) whenever the board asked for a filter; measured
		 * FILTER_CTRL_A 0x00100101 / FILTER_CTRL_B 0x00000000 on E1M-AEN803
		 * 2026W36-0002. On QEC channels (timer_id >= 12) this
		 * level-qualification claim does NOT hold -- SRC_0 has no A-AND-B
		 * qualification of any kind (measured, #2037; see the GAP note
		 * below), so do not read this paragraph as applying there.
		 *
		 * UNPROVEN AS A CURE: this is reasoned from the SVD, not measured.
		 * It is a candidate for the spurious count, not a demonstrated fix --
		 * see the file header for what the bench must show.
		 *
		 * GAP, still open after the SRC_0 fix landed and was measured
		 * (#2037): whether FILTER_CTRL_A/FILTER_CTRL_B affect the
		 * QEC_TRIGGER0..2 inputs on a QEC channel AT ALL is unproven either
		 * way. FILTER_CTRL_A/B (0x84/0x88) are documented in terms of
		 * "input A"/"input B", the same SRC_1-flavoured naming that turned
		 * out not to apply to these channels (see the RESOLVED note in the
		 * file header) -- so it is equally plausible that this write is
		 * inert on channel 12 and the 293-of-734 large-step bounce measured
		 * there is simply unfiltered contact bounce hitting an unqualified
		 * edge counter, OR that it does apply but asymmetrically between
		 * QEC_TRIGGER0 and QEC_TRIGGER1, which would be a second bias
		 * candidate. Not chased further this round.
		 */
		sys_write32(filt, UTIMER_FILTER_CTRL_A(timer_base));
		sys_write32(filt, UTIMER_FILTER_CTRL_B(timer_base));
	}

	if (cfg->timer_id >= 12) {
		/*
		 * QEC channels 12-15: SRC_0, not SRC_1.  hal_alif's
		 * alif_utimer_config_qdec_triggers() (drivers/utimer/src/utimer.c)
		 * only ever programs UP_1_SRC/DOWN_1_SRC -- correct for the
		 * lputimer0/1/2 instances Alif's own tree binds this same qdec
		 * driver to, wrong for QEC channels, whose real input path is
		 * SRC_0 (QDEC_SRC0_TRIG0_RISING comment above has the SVD + DFP
		 * demo citations, #2037).  Measured: an attended bench run had
		 * the raw P3_0/P3_1 pads toggling through all four quadrature
		 * states while UTIMER_CNTR stayed 0x00000000 the whole time with
		 * the old SRC_1 programming -- the signal reached the SoC and
		 * this channel never counted it.
		 */
		sys_write32(QDEC_SRC0_UP_VALUE, UTIMER_UP_0_SRC(timer_base));
		sys_write32(QDEC_SRC0_DOWN_VALUE, UTIMER_DOWN_0_SRC(timer_base));
	} else {
		/* Alif's own lputimer0/1/2 usage: SRC_1, "channel input A/B". */
		alif_utimer_config_qdec_triggers(timer_base);
	}

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
	 * DO NOT START THE COUNTER HERE.  This is deliberate, it is the opposite of
	 * what an earlier version of this driver did, and the reason is measured.
	 *
	 * A trigger-counting channel must NOT be started.  GLB_CNTR_START puts the
	 * channel into free-running clocked mode, where the counter advances on the
	 * peripheral clock rather than on quadrature events.  Measured on
	 * E1M-AEN803 serial 2026W36-0002 with an untouched encoder: after
	 * alif_utimer_start_counter() the counter advanced at 400,010,738 counts/s
	 * -- with UP_1_SRC and DOWN_1_SRC BOTH ZEROED, so no quadrature transition
	 * of any polarity could have contributed.  Writing GLB_CNTR_STOP froze it
	 * instantly and completely: three CNTR reads 5 s apart, bit-identical.
	 *
	 * With the shipped reload (CNTR_PTR = counts-per-revolution - 1 = 0x5F) that
	 * free-run wraps a whole revolution every 240 ns, so the reported angle was
	 * uncorrelated noise -- strictly worse than the stuck-at-zero symptom the
	 * start call was added to fix (#2037, cause of #2038).
	 *
	 * Alif's own QEC flow agrees and never starts the channel: qec0_app() in
	 * Boards/Templates/Baremetal/demo_qec.c calls ConfigCounter(TRIGGERING,
	 * TRIANGLE), SetCount, three ConfigTrigger calls, then reads GetCount and
	 * finally Stop -- there is no Start() anywhere in it.  Their MODE_TRIGGERING
	 * performs exactly one hardware action, utimer_glb_driver_output_disable().
	 * The counter increments from triggers alone.
	 *
	 * So CNTR_CTRL reading 0x00000021 here -- EN and CNTR_TRIG set, bit 1
	 * RUNNING clear -- and GLB_CNTR_RUNNING reading 0x00000000 are the CORRECT
	 * resting state for this channel, not evidence of a defect.  Note bit 1 is
	 * status, not control: it is set by GLB_CNTR_START and cleared by
	 * GLB_CNTR_STOP, and writing it into CNTR_CTRL does not take (measured --
	 * a write of 0x00000023 reads back 0x00000021).  hal_alif's utimer.h names
	 * this bit CNTR_CTRL_RUNNING (drivers/utimer/include/utimer.h:84-85) --
	 * that is the correct name.  The Alif DFP's OWN reference headers disagree
	 * with each other: alif-dfp-ref/drivers/include/utimer.h:86 calls the same
	 * bit CNTR_CTRL_START, while the AE822 SVD (:30573-30575) calls it
	 * CNTR_RUNNING and says "Writing this bit have no effect" -- matching the
	 * measurement above, not the DFP's own control-sounding name for it.
	 *
	 * OBSERVED (#2037, attended bench run): whether the counter increments on
	 * real quadrature edges in this resting state has now been measured with
	 * a hand on the shaft, and it does not -- the raw P3_0/P3_1 pads toggled
	 * through all four quadrature states while UTIMER_CNTR stayed
	 * 0x00000000 throughout.  The stuck-at-zero reading that started all of
	 * this was real, and "the counter was never started" was the wrong
	 * explanation for it; the SRC_0-vs-SRC_1 trigger-source fix above is the
	 * current explanation, pending its own bench confirmation.
	 */

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
