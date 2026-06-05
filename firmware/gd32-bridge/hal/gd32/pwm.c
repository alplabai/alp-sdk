/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- PWM output.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* PWM channels (TIMER0 + TIMER7).                                    */
/* ----------------------------------------------------------------- */

/* E1M PWM channel -> GD32 (timer, channel, output kind, pad, AF).
 * Sourced from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` for the
 * pad column; AF + timer-channel from the GD32G553xx Datasheet Rev2.0
 * Tables 2-10..2-13 (pin alternate-function summary).
 *
 *   PWM0  PA11  TIMER0_MCH0 (complement.) AF6
 *   PWM1  PB1   TIMER0_MCH2 (complement.) AF6
 *   PWM2  PB14  TIMER0_MCH1 (complement.) AF6
 *   PWM3  PC5   TIMER0_MCH3 (complement.) AF6
 *   PWM4  PC10  TIMER7_MCH0               AF4
 *   PWM5  PC11  TIMER7_MCH1               AF4
 *   PWM6  PC12  TIMER7_MCH2               AF4
 *   PWM7  PD0   TIMER7_MCH3               AF6
 *
 * Every PWM rides a distinct TIMER channel's complementary output,
 * so per-channel duty cycles are fully independent.  Each channel's
 * main output (CHx) is unused on V2N -- only the complementary
 * (CHxN) pad sits on the E1M PWM connector.
 *
 * Periods are SHARED across all PWMs of the same timer (TIMER0:
 * PWM0..3; TIMER7: PWM4..7) because each TIMER has one ARR.  The
 * per-channel `bridge_hw_pwm_set` body updates the timer's ARR every
 * call -- last write wins.  In typical V2N use the host sets the
 * same period across each group so this doesn't surface. */

const gd32_pwm_ch_t pwm_channels[] = {
    [0] = { TIMER0, TIMER_CH_0, true, GPIOA, GPIO_PIN_11, GPIO_AF_6 },
    [1] = { TIMER0, TIMER_CH_2, true, GPIOB, GPIO_PIN_1, GPIO_AF_6 },
    [2] = { TIMER0, TIMER_CH_1, true, GPIOB, GPIO_PIN_14, GPIO_AF_6 },
    [3] = { TIMER0, TIMER_CH_3, true, GPIOC, GPIO_PIN_5, GPIO_AF_6 },
    [4] = { TIMER7, TIMER_CH_0, true, GPIOC, GPIO_PIN_10, GPIO_AF_4 },
    [5] = { TIMER7, TIMER_CH_1, true, GPIOC, GPIO_PIN_11, GPIO_AF_4 },
    [6] = { TIMER7, TIMER_CH_2, true, GPIOC, GPIO_PIN_12, GPIO_AF_4 },
    [7] = { TIMER7, TIMER_CH_3, true, GPIOD, GPIO_PIN_0, GPIO_AF_6 },
};
_Static_assert(sizeof(pwm_channels) / sizeof(pwm_channels[0]) == PWM_CHANNEL_COUNT,
               "pwm_channels size must match PWM_CHANNEL_COUNT");

/* No read-back cache: bridge_hw_pwm_get reads CAR/CHxCV straight from
 * the timer so the host sees what the silicon is actually generating,
 * never a software echo of its own request (silicon lesson 2026-06-04:
 * a cache echo "verified" PWM for weeks while the pads were idle). */

/* Per-timer init.  Called once per peripheral from bridge_hw_init();
 * Advanced timers need timer_primary_output_config(ENABLE) before any
 * output pin actually drives (vs basic timers, where the channel
 * enable is sufficient). */
void pwm_timer_init(uint32_t periph)
{
    timer_parameter_struct ip;
    timer_struct_para_init(&ip);
    ip.prescaler         = (uint16_t)PWM_TIMER_PRESCALER;
    ip.alignedmode       = TIMER_COUNTER_EDGE;
    ip.counterdirection  = TIMER_COUNTER_UP;
    ip.period            = PWM_TIMER_ARR_MAX; /* 65.5 ms default; per-set */
    ip.clockdivision     = TIMER_CKDIV_DIV1;
    ip.repetitioncounter = 0u;
    timer_deinit(periph);
    timer_init(periph, &ip);
    timer_primary_output_config(periph, ENABLE);
    timer_enable(periph);
}

/* Per-channel init.  Sets PWM mode 0 (output high while counter <
 * compare) and 0 duty -- HW pad sits low until the host issues a
 * bridge_hw_pwm_set with a non-zero duty. */
void pwm_channel_init(const gd32_pwm_ch_t *ch)
{
    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    if (ch->complement) {
        oc.outputstate  = TIMER_CCX_DISABLE;
        oc.outputnstate = TIMER_CCXN_ENABLE;
    } else {
        oc.outputstate  = TIMER_CCX_ENABLE;
        oc.outputnstate = TIMER_CCXN_DISABLE;
    }
    oc.ocpolarity   = TIMER_OC_POLARITY_HIGH;
    oc.ocnpolarity  = TIMER_OCN_POLARITY_HIGH;
    oc.ocidlestate  = TIMER_OC_IDLE_STATE_LOW;
    oc.ocnidlestate = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(ch->periph, ch->channel, &oc);
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, 0u);
    timer_channel_output_mode_config(ch->periph, ch->channel, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(ch->periph, ch->channel, TIMER_OC_SHADOW_DISABLE);
}

int bridge_hw_pwm_set(uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (duty_ns > period_ns) return BRIDGE_HW_ERR_INVAL;

    /* Clear OPM if a prior bridge_hw_pwm_single_pulse left the timer
     * in one-pulse mode -- per the contract, a subsequent PWM_SET
     * returns the channel (and any other channels on the same timer)
     * to continuous output. */
    timer_single_pulse_mode_config(pwm_channels[channel].periph, TIMER_SP_MODE_REPETITIVE);

    /* Round period + duty to whole microseconds (the timer tick).
     * `period_us` must fit in 16 bits (ARR) -- caller is responsible
     * for staying under ~65 ms; we clamp on over-range so the timer
     * doesn't get an invalid value. */
    uint32_t period_us = period_ns / PWM_TIMER_TICK_NS;
    uint32_t duty_us   = duty_ns / PWM_TIMER_TICK_NS;
    if (period_us == 0u) return BRIDGE_HW_ERR_RANGE;
    if (period_us > PWM_TIMER_ARR_MAX + 1u) period_us = PWM_TIMER_ARR_MAX + 1u;
    if (duty_us > period_us) duty_us = period_us;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* ARR is "period_us - 1" because the up-counter counts 0..ARR
     * inclusive (period_us ticks total).  Updates ALL channels of
     * the same timer -- the contract documents this constraint. */
    timer_autoreload_value_config(ch->periph, (uint32_t)(period_us - 1u));
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, duty_us);
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    if (period_ns == 0 || duty_ns == 0) return BRIDGE_HW_ERR_INVAL;
    *period_ns = 0u;
    *duty_ns   = 0u;
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* Hardware read-back: convert the live CAR/CHxCV ticks to ns at
     * the fixed 1 us tick.  This reports what the pad is actually
     * doing -- including the shared-period reality (one ARR per
     * timer: a PWM_SET on a sibling channel moves this channel's
     * reported period too) and the boot default (65.536 ms period,
     * 0 duty) before the first PWM_SET.  CHxCV can legitimately read
     * ARR + 1 (single-pulse programs compare past the period for a
     * full-width pulse); clamp so duty never reports > period. */
    const uint32_t car = TIMER_CAR(ch->periph) & PWM_TIMER_ARR_MAX;
    uint32_t       cv  = timer_channel_capture_value_register_read(ch->periph, ch->channel);
    if (cv > car + 1u) cv = car + 1u;
    *period_ns = (car + 1u) * PWM_TIMER_TICK_NS;
    *duty_ns   = cv * PWM_TIMER_TICK_NS;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_configure(uint8_t channel, uint8_t align_mode, uint32_t dead_time_ns,
                            uint8_t break_cfg)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    /* v0.3 partial: accept the default settings that bridge_hw_init's
     * pwm_timer_init() programs (edge-aligned counter, no dead-time,
     * no break input) so the host's idempotent "set to defaults"
     * config calls succeed.  Non-defaults need timer-wide reconfigs
     * (CAM field via re-init, timer_break_config struct) that share
     * across all channels on the same timer; defer to a follow-up
     * with a per-timer apply path + last-write-wins semantics. */
    if (align_mode != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    if (dead_time_ns != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    if (break_cfg != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_single_pulse(uint8_t channel, uint32_t pulse_ns)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    uint32_t pulse_us = pulse_ns / PWM_TIMER_TICK_NS;
    if (pulse_us == 0u) return BRIDGE_HW_ERR_RANGE;
    if (pulse_us > PWM_TIMER_ARR_MAX + 1u) pulse_us = PWM_TIMER_ARR_MAX + 1u;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* Reset the timer counter so the pulse starts from t=0 then
     * program ARR = pulse_us (counts up; the channel output stays
     * high until the counter reaches the compare value).  Setting
     * compare = ARR + 1 keeps the output high through the entire
     * period so the pulse width matches `pulse_us`.  After ARR the
     * SP=SINGLE bit halts the timer until the next bridge_hw_pwm_set
     * or another bridge_hw_pwm_single_pulse re-arms it. */
    timer_counter_value_config(ch->periph, 0u);
    timer_autoreload_value_config(ch->periph, (uint32_t)(pulse_us - 1u));
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, pulse_us);
    timer_single_pulse_mode_config(ch->periph, TIMER_SP_MODE_SINGLE);
    timer_enable(ch->periph);

    /* A follow-up bridge_hw_pwm_get reads CAR/CHxCV directly and
     * reports duty == period for the one-shot (compare sits past the
     * period so the pulse spans the full window). */
    return BRIDGE_HW_OK;
}
