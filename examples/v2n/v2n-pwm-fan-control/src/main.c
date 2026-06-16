/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-pwm-fan-control -- ramp the portable `E1M_PWM0` channel
 * along a fan-speed curve.  Demonstrates the canonical
 * `alp_pwm_open` + `alp_pwm_set_duty` usage on the V2N module,
 * walking duty cycle through a five-stop curve so the wave shape
 * is observable on a scope.
 *
 * Why E1M-V2N for the fan-control case study?  V2N modules carry
 * the Renesas RZ/V2N (~5-8 W typical) plus a DEEPX DX-M1 NPU on
 * the M1 SKUs (~10-15 W under load), so a board-driven fan is
 * a normal accessory.  The same code works on any E1M-conformant
 * SoM that exposes `E1M_PWM0` -- the SDK routes the call to
 * whichever silicon physically drives that pad on the active SoM
 * (Alif GPT on AEN, GD32 IO-MCU on V2N, NXP TPU on i.MX 93).
 * Application code never names a specific peripheral block; the
 * portable surface is the `<alp/pwm.h>` API + the `E1M_PWM*`
 * instance IDs from `<alp/e1m_pinout.h>`.
 *
 * The example treats PWM channel 0 as the fan-control output.  A
 * production firmware would read the board's thermistor /
 * temp-sensor + adjust duty in a control loop -- this example uses
 * a fixed five-stop ramp so the staircase is visible on a scope
 * without needing live sensor data.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/pwm.h"
#include "alp/e1m_pinout.h"

/* Fan-curve setpoints: each row is (duty_percent_x10, dwell_ms).
 * The x10 scaling gives one decimal of precision without floats. */
typedef struct {
	uint16_t duty_pct_x10; /**< 0..1000  -- 0 = stopped, 1000 = full speed */
	uint16_t dwell_ms;
} fan_curve_step_t;

static const fan_curve_step_t fan_curve[] = {
	{ 0u, 500u },     /* fan off: cold thermistor case */
	{ 300u, 750u },   /* low:    e.g. CPU at 50 C */
	{ 600u, 750u },   /* medium: e.g. CPU at 65 C */
	{ 850u, 750u },   /* high:   e.g. CPU at 75 C */
	{ 1000u, 1000u }, /* max:    e.g. CPU at 80 C+ */
};

/* PWM period.  25 kHz keeps the board above the audible range so
 * a 4-wire fan's tach line stays clean.  40 us = 25 kHz. */
#define FAN_PWM_PERIOD_NS 40000u

int main(void)
{
	printf("[fan] v2n-pwm-fan-control (portable <alp/pwm.h>)\n");

	/* Open the portable PWM0 channel.  No SPI / I2C setup, no
     * chip driver references -- the SDK resolves which silicon
     * block physically drives the pad on this SoM and dispatches
     * `alp_pwm_*` accordingly.  On V2N that goes through the GD32
     * IO-MCU bridge internally; on AEN through the Alif GPT; on
     * i.MX 93 through the NXP TPU.  None of that surfaces here. */
	alp_pwm_t *fan = alp_pwm_open(&(alp_pwm_config_t){
	    .channel_id = E1M_PWM0,
	    .period_ns  = FAN_PWM_PERIOD_NS,
	    .polarity   = ALP_PWM_POLARITY_NORMAL,
	});
	if (fan == NULL) {
		printf("[fan] alp_pwm_open failed: alp_last_error=%d "
		       "(NOT_READY on native_sim; check board.yaml on real HW)\n",
		       (int)alp_last_error());
		return 0;
	}

	/* Walk the fan curve forever.  Real firmware substitutes a
     * thermistor reading + closed-loop control -- the curve below
     * shows the wave shape a customer would observe on a scope. */
	for (;;) {
		for (size_t i = 0u; i < ARRAY_SIZE(fan_curve); ++i) {
			const fan_curve_step_t *step = &fan_curve[i];
			/* duty_ns = period_ns * duty_pct_x10 / 1000.  Integer
             * math: period_ns * duty_pct_x10 fits in 32 bits for
             * any duty up to 100 % at 40 us period. */
			const uint32_t duty_ns = (uint32_t)((FAN_PWM_PERIOD_NS * step->duty_pct_x10) / 1000u);
			alp_status_t   s       = alp_pwm_set_duty(fan, duty_ns);
			if (s != ALP_OK) {
				printf("[fan] alp_pwm_set_duty -> %d (duty=%u/1000)\n", (int)s,
				       (unsigned)step->duty_pct_x10);
				/* Don't bail -- the backend may be transiently busy;
                 * just keep walking the curve and let the next
                 * setpoint succeed. */
			} else {
				printf("[fan] duty=%u/1000  period=%u ns  duty_ns=%u\n",
				       (unsigned)step->duty_pct_x10, (unsigned)FAN_PWM_PERIOD_NS,
				       (unsigned)duty_ns);
			}
			k_msleep(step->dwell_ms);
		}
	}

	/* Unreachable in the demo -- for completeness, the teardown a
     * real app does when the controller shuts down: drop duty to 0
     * then close the handle.  alp_pwm_close drives the output low. */
	(void)alp_pwm_set_duty(fan, 0u);
	alp_pwm_close(fan);
	return 0;
}
