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
 * docs/adr/0017 + task #21.
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
#define UTIMER_DRIVER_A 0U
#define UTIMER_DRIVER_B 1U
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

static int pwm_alif_get_cycles_per_sec(const struct device *dev, uint32_t channel, uint64_t *cycles)
{
    const struct pwm_alif_config *config = dev->config;

    ARG_UNUSED(channel);

    *cycles = (uint64_t)config->clock_freq;
    return 0;
}

static int pwm_alif_set_cycles(const struct device *dev, uint32_t channel, uint32_t period_cycles,
                               uint32_t pulse_cycles, pwm_flags_t flags)
{
    const struct pwm_alif_config *config = dev->config;
    /* The hal_alif UTIMER API takes raw 32-bit register bases; the UTIMER
	 * window is within the 32-bit address space on the M55 cores. */
    uint32_t timer_base = (uint32_t)DEVICE_MMIO_GET(dev);
    uint32_t glb_base   = (uint32_t)config->global_base;
    uint8_t  driver;
    uint32_t drv_cfg;

    if (channel >= UTIMER_NUM_DRIVERS) {
        return -EINVAL;
    }
    driver = (uint8_t)channel;

    /* The UTIMER reload register is the period; the compare register is the
	 * edge where the output flips.  A 0..period up-counter with the driver
	 * starting LOW and flipping HIGH at the compare match would invert the
	 * intended duty (high time = period - pulse).  We instead start the
	 * cycle HIGH and drop LOW at the compare match, so compare = pulse gives
	 * a HIGH time of `pulse` cycles.  PWM_POLARITY_INVERTED swaps the start
	 * and match levels. */
    const bool inverted = (flags & PWM_POLARITY_INVERTED) != 0;

    /* Stop + re-program: the buffered reload/compare write is latched on the
	 * next cycle boundary, but a clean stop keeps the first cycle correct on
	 * a live frequency change. */
    alif_utimer_stop_counter(glb_base, config->timer_id);

    /* Ensure the per-timer clock + up-counter (sawtooth) direction. */
    alif_utimer_enable_timer_clock(glb_base, config->timer_id);
    alif_utimer_set_up_counter(timer_base);
    alif_utimer_enable_soft_counter_ctrl(timer_base);

    /* Full-off: pulse == 0 -> drive output to the inactive level and leave
	 * the counter stopped.  Disable the driver output so the pad sits at the
	 * disable level. */
    if (pulse_cycles == 0U) {
        alif_utimer_disable_driver(timer_base, driver);
        alif_utimer_disable_driver_output(timer_base, driver, config->timer_id);
        return 0;
    }

    /* Program period (reload) and duty (compare). */
    alif_utimer_set_counter_reload_value(timer_base, period_cycles);

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

    /* Arm the counter and start it from the global control block. */
    alif_utimer_enable_counter(timer_base);
    alif_utimer_start_counter(glb_base, config->timer_id);

    return 0;
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
#define PWM_DEVICE_INIT(inst)                                                                      \
    PINCTRL_DT_INST_DEFINE(inst);                                                                  \
    static struct pwm_alif_data         pwm_alif_data_##inst;                                      \
    static const struct pwm_alif_config pwm_alif_config_##inst = {                                 \
        DEVICE_MMIO_ROM_INIT(DT_INST_PARENT(inst)),                                                \
        .global_base = DT_REG_ADDR_BY_IDX(DT_INST_PARENT(inst), 1),                                \
        .clock_freq  = DT_PROP_OR(DT_INST_PARENT(inst), clock_frequency, UTIMER_DEFAULT_CLK_HZ),   \
        .timer_id    = DT_PROP(DT_INST_PARENT(inst), timer_id),                                    \
        .pcfg        = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, pwm_alif_init, NULL, &pwm_alif_data_##inst,                        \
                          &pwm_alif_config_##inst, POST_KERNEL, CONFIG_PWM_INIT_PRIORITY,          \
                          &pwm_alif_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_DEVICE_INIT)
