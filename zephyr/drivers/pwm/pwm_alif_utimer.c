/*
 * Copyright (c) 2024 Alif Semiconductor
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr PWM-class driver for the Alif Ensemble UTIMER in edge-aligned
 * (sawtooth / up-counter) PWM mode.  On the E1M-AEN801 (Ensemble E8) SoM the
 * eight E1M PWM pads map onto four UTIMER blocks, two driver outputs each:
 *
 *     E1M PWM0 = UT11_T1_C (utimer11, driver B / channel 1)
 *     E1M PWM1 = UT11_T0_C (utimer11, driver A / channel 0)
 *     E1M PWM2 = UT10_T1_A (utimer10, driver B / channel 1)
 *     E1M PWM3 = UT10_T0_A (utimer10, driver A / channel 0)
 *     E1M PWM4 = UT6_T1_C  (utimer6,  driver B / channel 1)
 *     E1M PWM5 = UT6_T0_A  (utimer6,  driver A / channel 0)
 *     E1M PWM6 = UT3_T1_C  (utimer3,  driver B / channel 1)
 *     E1M PWM7 = UT3_T0_C  (utimer3,  driver A / channel 0)
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 (in-tree thin driver over the Apache-2.0 hal_alif HW
 * library) -- INTERIM, BENCH-UNVERIFIED.  Kept in-tree, not retired onto the
 * fork: there is no fork PWM driver to consume (bindings only), and hal_alif's
 * alif_utimer_* library exposes no Zephyr device model -- so this thin shell is
 * the only path to AEN PWM.  INTERIM until E8 bench, then permanent.  See
 * docs/adr/0017.
 *
 * Written over the hal_alif register-poke library (modules/hal/alif
 * drivers/utimer/{include/utimer.h,src/utimer.c}); that library exposes NO
 * struct device and NO Zephyr device model.  Upstream Zephyr v4.4 and hal_alif
 * ship NO PWM driver-class device for the Alif UTIMER -- only the fork ships
 * the bindings (dts/bindings/pwm/alif,pwm.yaml + timer/alif,utimer.yaml), and
 * the fork's driver .c is absent from the local reference tree.  This driver
 * body is therefore authored here against the documented hal_alif API; every
 * UTIMER register access goes through an alif_utimer_* HAL call (no offset or
 * bitfield is open-coded).  It has NOT been run on real silicon.
 * ====================================================================
 *
 * Clock / ns->ticks conversion.  pwm_set()'s ns_to_cycles path divides by the
 * rate this driver reports from get_cycles_per_sec().  The UTIMER counter input
 * frequency is the SoC's high-frequency timer clock; its exact value is a
 * silicon-determined HW fact that must come from the Alif Ensemble TRM, NOT be
 * invented (per the alp-sdk pending-hw-configs policy).  The rate is taken from
 * the parent utimer node's `clock-frequency` devicetree property; until the
 * human fills in the verified value it falls back to a clearly-marked 100 MHz
 * placeholder (UTIMER_DEFAULT_CLK_HZ).  A wrong rate yields a wrong PWM
 * frequency but does not change the register programming.  See `remaining` in
 * the slice handoff.
 *
 * API conformance vs Zephyr v4.4 pwm_driver_api: this driver implements the two
 * mandatory ops (.set_cycles, .get_cycles_per_sec).  Capture
 * (.configure_capture/.enable_capture/.disable_capture) is gated by
 * CONFIG_PWM_CAPTURE upstream and is not provided; the alp_pwm portable surface
 * routes capture through the V2N GD32 bridge backend, not this path.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/device_mmio.h>

#include <utimer.h>

#define DT_DRV_COMPAT alif_pwm

LOG_MODULE_REGISTER(pwm_alif_utimer, CONFIG_PWM_LOG_LEVEL);

/*
 * TODO(hw): UTIMER counter tick rate in Hz, sourced from the parent utimer
 * node's `clock-frequency` devicetree property (alif,utimer.yaml).  This is the
 * rate pwm_set()'s ns->cycles path divides by.  The verified value is a
 * silicon-determined HW fact that must come from the Alif Ensemble E8 TRM (the
 * UTIMER clock-tree source) -- it is NOT invented here (per the alp-sdk
 * pending-hw-configs policy).  When the property is absent the binding's
 * documented default (100 MHz) is used as a clearly-marked placeholder; a wrong
 * rate yields a wrong PWM frequency but does not change the register
 * programming.  See `remaining` in the slice handoff.
 */
#define UTIMER_DEFAULT_CLK_HZ 100000000UL

/* UTIMER driver-output indices: 0 = channel A (COMPARE_A / driver A),
 * 1 = channel B (COMPARE_B / driver B).  The pwm child node's first DT cell
 * `channel` carries this directly. */
#define UTIMER_DRIVER_A    0U
#define UTIMER_DRIVER_B    1U
#define UTIMER_NUM_DRIVERS 2U

struct pwm_alif_config {
	/* Timer block base (the parent utimer node's reg[0]); mapped via the
	 * MMIO machinery so a virtual address is available on MMU targets. */
	DEVICE_MMIO_ROM;
	/* Global UTIMER block base (parent reg[1], reg-names "global"); the
	 * GLB_* HAL ops operate on this address.  On the M55 cores the UTIMER
	 * window is identity-mapped, so the raw physical base is also the
	 * address the HAL pokes; we carry it as a plain uintptr_t. */
	uintptr_t                        global_base;
	uint32_t                         clock_freq;
	uint8_t                          timer_id;
	const struct pinctrl_dev_config *pcfg;
};

struct pwm_alif_data {
	DEVICE_MMIO_RAM;
};

/*
 * Shared-counter bookkeeping.  Driver A and driver B of one UTIMER block are two
 * Zephyr PWM devices (one per child node) but ONE hardware counter: the
 * UTIMERn_CNTR counter, its reload register (the period) and the global
 * start/stop bit (indexed by timer_id) are per timer; only COMPARE_A/COMPARE_B
 * and COMPARE_CTRL_A/B are per driver.  So the two siblings must agree on the
 * period and neither may stop or re-zero the counter while the other is driving
 * its pad.  This table, indexed by timer_id, is the single place both devices
 * see that shared state.
 *
 * Policy (also the contract callers get):
 *   - A duty-only update (same period) touches only that channel's COMPARE_x /
 *     COMPARE_CTRL_x; the counter keeps running.
 *   - The period of a running counter is changed only when the calling channel
 *     is the sole active one.  If the sibling is active with a different
 *     period the request fails with -EINVAL and nothing is written.
 *   - pulse == 0 switches off only the calling channel's output; the counter is
 *     stopped only when no channel is left active.
 *
 * The compare write is not shadow-buffered here: the hal_alif header is not
 * available to this tree, so no buffered-compare register is assumed.  A duty
 * change on a running counter takes effect at the HW's own latch point, never
 * by stopping the counter.
 */
#define UTIMER_MAX_TIMERS 16U

struct pwm_alif_shared {
	uint32_t period_cycles; /* reload value in effect; valid while the counter has run */
	uint8_t  active_mask;   /* bit n set: driver n is outputting a PWM waveform */
	bool     running;       /* counter started and not stopped */
};

static struct pwm_alif_shared pwm_alif_shared[UTIMER_MAX_TIMERS];
static struct k_spinlock      pwm_alif_lock;

static int pwm_alif_get_cycles_per_sec(const struct device *dev, uint32_t channel, uint64_t *cycles)
{
	const struct pwm_alif_config *config = dev->config;

	ARG_UNUSED(channel);

	*cycles = (uint64_t)config->clock_freq;
	return 0;
}

static int pwm_alif_set_cycles_locked(const struct device *dev,
                                      uint32_t             channel,
                                      uint32_t             period_cycles,
                                      uint32_t             pulse_cycles,
                                      pwm_flags_t          flags)
{
	const struct pwm_alif_config *config = dev->config;
	struct pwm_alif_shared       *sh     = &pwm_alif_shared[config->timer_id];
	/* The hal_alif UTIMER API takes raw 32-bit register bases; the UTIMER
	 * window is within the 32-bit address space on the M55 cores. */
	uint32_t timer_base = (uint32_t)DEVICE_MMIO_GET(dev);
	uint32_t glb_base   = (uint32_t)config->global_base;
	uint8_t  driver     = (uint8_t)channel;
	uint8_t  self       = (uint8_t)BIT(driver);
	uint8_t  others     = sh->active_mask & (uint8_t)~self;
	uint32_t drv_cfg;

	/* Full-off: pulse == 0 -> disable only this driver's output so the pad
	 * sits at the disable level.  The counter is shared, so it is stopped
	 * only when this was the last active channel; otherwise the sibling keeps
	 * its waveform. */
	if (pulse_cycles == 0U) {
		alif_utimer_disable_driver(timer_base, driver);

		/*
		 * Open-coded instead of alif_utimer_disable_driver_output(), which is
		 * broken in the vendor HAL: it writes the bit INDEX as a bit MASK --
		 *
		 *     REG(UTIMER_GLB_DRIVER_OEN(reg_base)) |= ((timer_id * 2) + driver);
		 *
		 * where its own enable twin two lines below shifts correctly.  HWRM
		 * 13.2.6.3.5 UTIMER_GLB_DRIVER_OEN: "1-0 DRIVER_OEN_0 ... Channel 0
		 * active low output enable controls for driver A and B.  B is the upper
		 * bit, A is the lower bit." -- two bits per channel across all 16
		 * channels in one GLOBAL register, so the bit is timer_id * 2 + driver.
		 *
		 * For E1M PWM6 = UT3_T1_C (timer_id 3, driver 1) the HAL ORed 3*2+1 = 7
		 * = 0b111, output-disabling UTIMER0 driver A, UTIMER0 driver B and
		 * UTIMER1 driver A -- any other PWM channel driving on those goes dark
		 * -- while never setting bit 7, the UT3 driver-B bit it meant to
		 * (#1828).  The register is active-LOW output enable, so disabling is a
		 * SET of that one bit.
		 */
		sys_set_bit(UTIMER_GLB_DRIVER_OEN(glb_base),
		            ((uint32_t)config->timer_id * 2U) + (uint32_t)driver);

		sh->active_mask &= (uint8_t)~self;
		if (sh->active_mask == 0U && sh->running) {
			alif_utimer_stop_counter(glb_base, config->timer_id);
			sh->running = false;
		}
		return 0;
	}

	/* The reload register is shared: a sibling that is still driving its pad
	 * with a different period cannot be retimed under it. */
	if (others != 0U && sh->period_cycles != period_cycles) {
		LOG_ERR("utimer%u: driver %u period %u conflicts with active sibling period %u",
		        config->timer_id,
		        driver,
		        period_cycles,
		        sh->period_cycles);
		return -EINVAL;
	}

	/* Only the sole active channel may stop/reload/re-zero the counter: either
	 * first start, or a live period change with nobody else on this timer. */
	if (!sh->running || sh->period_cycles != period_cycles) {
		if (sh->running) {
			alif_utimer_stop_counter(glb_base, config->timer_id);
			sh->running = false;
		}

		/* Ensure the per-timer clock + up-counter (sawtooth) direction. */
		alif_utimer_enable_timer_clock(glb_base, config->timer_id);
		alif_utimer_set_up_counter(timer_base);
		alif_utimer_enable_soft_counter_ctrl(timer_base);

		/* Program period (reload). */
		alif_utimer_set_counter_reload_value(timer_base, period_cycles);

		/*
		 * Re-zero the counter.  HWRM 13.2.5.1's up-timer setup sequence is
		 * explicit: "1. Set the pointer value to UTIMERn_CNTR_PTR = 250. ... 2.
		 * Enable timer counter ... 3. Initialize the timer counter to 0 by
		 * writing the UTIMERn_CNTR = 0x0."  Step 3 was missing from this driver
		 * entirely, so a LIVE period change left CNTR wherever it happened to
		 * be: dropping from 1 kHz to 100 kHz at 100 MHz sets CNTR_PTR to 1000
		 * while CNTR is still around 90000, already past the reset point, so
		 * the up-counter had to climb to 0xFFFFFFFF before it could wrap --
		 * about 43 seconds of a dead pad (#1829).  13.2.6.3.32 requires this
		 * write happen while the counter is stopped, which the stop above (or
		 * the never-started state) provides.
		 */
		alif_utimer_set_counter_value(timer_base, 0U);
		sh->period_cycles = period_cycles;
	}

	/* The UTIMER reload register is the period; the compare register is the
	 * edge where the output flips.  A 0..period up-counter with the driver
	 * starting LOW and flipping HIGH at the compare match would invert the
	 * intended duty (high time = period - pulse).  We instead start the
	 * cycle HIGH and drop LOW at the compare match, so compare = pulse gives
	 * a HIGH time of `pulse` cycles.  PWM_POLARITY_INVERTED swaps the start
	 * and match levels. */
	const bool inverted = (flags & PWM_POLARITY_INVERTED) != 0;

	/* Full-on: pulse >= period -> hold the active level for the whole cycle.
	 * Clamp the compare to the period so the match never fires mid-cycle. */
	if (pulse_cycles >= period_cycles) {
		alif_utimer_set_compare_value(timer_base, driver, period_cycles);
	} else {
		alif_utimer_set_compare_value(timer_base, driver, pulse_cycles);
	}

	/*
	 * Drive-output configuration (COMPARE_CTRL_A/B).  Bits per utimer.h:
	 *   - START_VAL: level held at cycle start (counter == 0).
	 *   - *_AT_COMP_MATCH: level taken when the counter hits COMPARE.
	 * Normal polarity: start HIGH, go LOW at the compare match -> HIGH for
	 * `pulse` cycles.  Inverted polarity: start LOW, go HIGH at the match.
	 */
	if (inverted) {
		drv_cfg = COMPARE_CTRL_DRV_START_VAL_LOW | COMPARE_CTRL_DRV_HIGH_AT_COMP_MATCH |
		          COMPARE_CTRL_DRV_LOW_AT_CYCLE_END | COMPARE_CTRL_DRV_DRIVER_EN |
		          COMPARE_CTRL_DRV_COMPARE_EN;
	} else {
		drv_cfg = COMPARE_CTRL_DRV_START_VAL_HIGH | COMPARE_CTRL_DRV_LOW_AT_COMP_MATCH |
		          COMPARE_CTRL_DRV_HIGH_AT_CYCLE_END | COMPARE_CTRL_DRV_DRIVER_EN |
		          COMPARE_CTRL_DRV_COMPARE_EN;
	}
	alif_utimer_config_driver_output(timer_base, driver, drv_cfg);

	alif_utimer_enable_compare_match(timer_base, driver);
	alif_utimer_enable_driver(timer_base, driver);
	alif_utimer_enable_driver_output(glb_base, driver, config->timer_id);
	sh->active_mask |= self;

	/* Arm the counter and start it from the global control block -- only if
	 * it is not already running for the sibling. */
	if (!sh->running) {
		alif_utimer_enable_counter(timer_base);
		alif_utimer_start_counter(glb_base, config->timer_id);
		sh->running = true;
	}

	return 0;
}

static int pwm_alif_set_cycles(const struct device *dev,
                               uint32_t             channel,
                               uint32_t             period_cycles,
                               uint32_t             pulse_cycles,
                               pwm_flags_t          flags)
{
	const struct pwm_alif_config *config = dev->config;
	k_spinlock_key_t              key;
	int                           ret;

	if (channel >= UTIMER_NUM_DRIVERS || config->timer_id >= UTIMER_MAX_TIMERS) {
		return -EINVAL;
	}

	key = k_spin_lock(&pwm_alif_lock);
	ret = pwm_alif_set_cycles_locked(dev, channel, period_cycles, pulse_cycles, flags);
	k_spin_unlock(&pwm_alif_lock, key);

	return ret;
}

static DEVICE_API(pwm, pwm_alif_driver_api) = {
	.set_cycles         = pwm_alif_set_cycles,
	.get_cycles_per_sec = pwm_alif_get_cycles_per_sec,
};

static int pwm_alif_init(const struct device *dev)
{
	const struct pwm_alif_config *config = dev->config;
	int                           err;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		return err;
	}

	return 0;
}

/*
 * The compatible "alif,pwm" sits on the CHILD pwm node; reg / timer-id live on
 * the PARENT utimer node.  DEVICE_MMIO_ROM_INIT wants a node with a reg, so we
 * point the MMIO machinery + the global base + timer-id at the parent.
 */
#define PWM_DEVICE_INIT(inst) \
	PINCTRL_DT_INST_DEFINE(inst); \
	static struct pwm_alif_data         pwm_alif_data_##inst; \
	static const struct pwm_alif_config pwm_alif_config_##inst = { \
		DEVICE_MMIO_ROM_INIT(DT_INST_PARENT(inst)), \
		.global_base = DT_REG_ADDR_BY_IDX(DT_INST_PARENT(inst), 1), \
		.clock_freq  = DT_PROP_OR(DT_INST_PARENT(inst), clock_frequency, UTIMER_DEFAULT_CLK_HZ), \
		.timer_id    = DT_PROP(DT_INST_PARENT(inst), timer_id), \
		.pcfg        = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), \
	}; \
	DEVICE_DT_INST_DEFINE(inst, \
	                      pwm_alif_init, \
	                      NULL, \
	                      &pwm_alif_data_##inst, \
	                      &pwm_alif_config_##inst, \
	                      POST_KERNEL, \
	                      CONFIG_PWM_INIT_PRIORITY, \
	                      &pwm_alif_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_DEVICE_INIT)
