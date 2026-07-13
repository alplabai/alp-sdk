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

/* Sticky per-TIMER counter-alignment mode set by bridge_hw_pwm_configure.
 * Index 0 = TIMER0 (PWM0..3), index 1 = TIMER7 (PWM4..7); value is the
 * wire align_mode (0 edge / 1 center-up / 2 center-down / 3 center-both,
 * matching alp_pwm_align_t).  CAM is a timer-wide field shared by every
 * channel on the timer, so this is per-timer, not per-channel:
 * last-write-wins across sibling channels (documented in the protocol
 * spec).  bridge_hw_pwm_set / _get read it to convert between commanded
 * period/duty and the ARR/compare ticks, because a center-aligned
 * counter runs 0->ARR->0 (period == 2*ARR ticks) where an edge-aligned
 * counter runs 0->ARR (period == ARR+1 ticks). */
static uint8_t pwm_align_mode[2];

/* TIMER base -> pwm_align_mode index. */
static uint8_t pwm_timer_index(uint32_t periph)
{
	return (periph == TIMER0) ? 0u : 1u;
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

	/* Round period + duty to whole microseconds (the timer tick). */
	uint32_t period_us = period_ns / PWM_TIMER_TICK_NS;
	uint32_t duty_us   = duty_ns / PWM_TIMER_TICK_NS;
	if (period_us == 0u) return BRIDGE_HW_ERR_RANGE;

	const gd32_pwm_ch_t *ch = &pwm_channels[channel];

	/* Convert commanded period/duty to ARR + compare, honouring the
	 * timer's configured alignment (bridge_hw_pwm_configure).  The
	 * up-counter (edge) counts 0..ARR inclusive -> period == ARR+1
	 * ticks and high-time == compare ticks.  A center-aligned counter
	 * runs 0->ARR->0 -> period == 2*ARR ticks and high-time ==
	 * 2*compare ticks (compare on the up-ramp + compare on the
	 * down-ramp), so both ARR and compare are the commanded value
	 * halved.  ARR must fit in 16 bits either way; clamp on over-range
	 * so the timer never gets an invalid reload. */
	uint32_t arr, cmp;
	if (pwm_align_mode[pwm_timer_index(ch->periph)] == 0u) {
		if (period_us > PWM_TIMER_ARR_MAX + 1u) period_us = PWM_TIMER_ARR_MAX + 1u;
		if (duty_us > period_us) duty_us = period_us;
		arr = period_us - 1u;
		cmp = duty_us;
	} else {
		uint32_t half_period = period_us / 2u;
		uint32_t half_duty   = duty_us / 2u;
		if (half_period == 0u) return BRIDGE_HW_ERR_RANGE; /* period < 2 us */
		if (half_period > PWM_TIMER_ARR_MAX) half_period = PWM_TIMER_ARR_MAX;
		if (half_duty > half_period) half_duty = half_period;
		arr = half_period;
		cmp = half_duty;
	}

	/* Updates ALL channels of the same timer -- the contract documents
	 * this shared-ARR constraint. */
	timer_autoreload_value_config(ch->periph, arr);
	timer_channel_output_pulse_value_config(ch->periph, ch->channel, cmp);
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
     * full-width pulse); clamp so duty never reports > period.
     *
     * The tick->ns conversion mirrors bridge_hw_pwm_set's alignment
     * math: edge-aligned period is CAR+1 ticks, center-aligned is
     * 2*CAR ticks (and duty 2*CV), so a channel configured
     * center-aligned reads back the same period/duty it was
     * commanded. */
	const uint32_t car = TIMER_CAR(ch->periph) & PWM_TIMER_ARR_MAX;
	uint32_t       cv  = timer_channel_capture_value_register_read(ch->periph, ch->channel);
	if (cv > car + 1u) cv = car + 1u;
	if (pwm_align_mode[pwm_timer_index(ch->periph)] == 0u) {
		*period_ns = (car + 1u) * PWM_TIMER_TICK_NS;
		*duty_ns   = cv * PWM_TIMER_TICK_NS;
	} else {
		*period_ns = 2u * car * PWM_TIMER_TICK_NS;
		*duty_ns   = 2u * cv * PWM_TIMER_TICK_NS;
		if (*duty_ns > *period_ns) *duty_ns = *period_ns;
	}
	return BRIDGE_HW_OK;
}

int bridge_hw_pwm_configure(uint8_t  channel,
                            uint8_t  align_mode,
                            uint32_t dead_time_ns,
                            uint8_t  break_cfg)
{
	if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

	/* align_mode 0..3 map to CTL0.CAM (0 edge, 1 center-up, 2
	 * center-down, 3 center-both -- alp_pwm_align_t).  Reject out of
	 * range before touching hardware so a bad value leaves the timer
	 * untouched. */
	if (align_mode > 3u) return BRIDGE_HW_ERR_INVAL;

	/* dead_time_ns and break_cfg are advanced-timer features the GD32
	 * TIMER0/7 silicon HAS, but that the V2N E1M routing does not
	 * physically expose, so honouring them would be an unverifiable
	 * register write (exactly the "cache echo verified PWM while the
	 * pads were idle" trap this file warns about above):
	 *   - Dead-time is the guard gap between a channel's main (CHx) and
	 *     complementary (CHxN) outputs, but only the CHxN pad sits on
	 *     the E1M PWM connector (see the channel-map comment) -- there
	 *     is no complementary PAIR on the wire for a dead-time to act
	 *     on, so any DTCFG value is physically inert.
	 *   - The break input needs a BRK pad, and the V2N gd32-io-mcu-map
	 *     routes none -- the break logic could be armed but nothing
	 *     could ever trigger it.
	 * Both therefore return NOSUPPORT on this board (a hardware-routing
	 * limit, not an unimplemented feature); a future carrier that
	 * routes the complementary pair / a BRK pad would lift this.
	 * align_mode == 0 with both zero is still the idempotent
	 * "set-to-defaults" call and succeeds. */
	if (dead_time_ns != 0u) return BRIDGE_HW_ERR_NOTIMPL;
	if (break_cfg != 0u) return BRIDGE_HW_ERR_NOTIMPL;

	const gd32_pwm_ch_t *ch  = &pwm_channels[channel];
	const uint8_t        idx = pwm_timer_index(ch->periph);
	if (pwm_align_mode[idx] == align_mode) return BRIDGE_HW_OK; /* no change */

	/* CAM is a shared timer-wide field and the GD32 (like most STM32-
	 * lineage advanced timers) latches it only while the counter is
	 * stopped, so bracket the change in disable/enable.  This briefly
	 * halts every channel on the timer -- acceptable at config time,
	 * and last-write-wins across the sibling channels is the documented
	 * contract.  The counter value + ARR/compare survive the toggle, so
	 * a running duty resumes with the new alignment on the next
	 * update. */
	static const uint32_t cam_map[4] = {
		TIMER_COUNTER_EDGE,        /* 0: edge          */
		TIMER_COUNTER_CENTER_UP,   /* 1: center-up     */
		TIMER_COUNTER_CENTER_DOWN, /* 2: center-down   */
		TIMER_COUNTER_CENTER_BOTH, /* 3: center-both   */
	};
	timer_disable(ch->periph);
	uint32_t ctl0 = TIMER_CTL0(ch->periph);
	ctl0 &= ~(uint32_t)TIMER_CTL0_CAM;
	ctl0 |= cam_map[align_mode];
	TIMER_CTL0(ch->periph) = ctl0;
	timer_enable(ch->periph);

	pwm_align_mode[idx] = align_mode;
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
