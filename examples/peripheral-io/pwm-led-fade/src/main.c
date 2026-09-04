/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * pwm-led-fade — fade the board's green LED via BOARD_PWM_LED_GREEN
 * from 0 % to 100 % and back, demonstrating the canonical alp_pwm_*
 * usage pattern.
 *
 * On native_sim there's no PWM emul controller, so alp_pwm_open
 * returns NULL with last_error = ALP_ERR_NOT_READY.  The example
 * prints the diagnostic and exits cleanly so the twister console
 * harness can assert it ran.
 *
 * Runs on both EVKs: BOARD_PWM_LED_GREEN (from <alp/board.h>)
 * resolves to ALP_E1M_PWM3 on E1M EVK (RGB LED green via PWM3 --
 * bench-measured 2026-07-28, see metadata/boards/e1m-evk.yaml) and
 * ALP_E1M_X_PWM7 on E1M-X EVK (RGB LED green via PWM7).  On real
 * silicon the LED breathes at 1 kHz across a linear duty sweep.
 */

#include <stdio.h>

#include "alp/peripheral.h"

#include "alp/pwm.h"

/* BOARD_PWM_LED_GREEN is the portable alias from <alp/board.h>
 * (ALP_E1M_PWM3 on E1M EVK; ALP_E1M_X_PWM7 on E1M-X EVK). */
#include "alp/board.h"

#define PERIOD_NS     1000000u /* 1 kHz */
#define STEPS         50
#define STEP_DELAY_MS 20

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[pwm] open BOARD_PWM_LED_GREEN (period=%u ns)\n", PERIOD_NS);

	alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
	    .channel_id =
	        BOARD_PWM_LED_GREEN, /* ALP_E1M_PWM3 on E1M EVK; ALP_E1M_X_PWM7 on E1M-X EVK */
	    .period_ns = PERIOD_NS,
	    .polarity  = ALP_PWM_POLARITY_NORMAL,
	});
	if (led == NULL) {
		/* CONFIG_BOARD_NATIVE_SIM is the only target this example can
		 * actually tell apart at compile time -- there's no PWM emul
		 * controller there, so NOT_READY is expected.  On every other
		 * target (including real silicon) the cause is unknown from
		 * here, so say nothing about it rather than misdirect (#1375:
		 * this line used to blame "no PWM emul" even when running on
		 * an E8 with a genuinely missing alp-pwm3 DT alias). */
#ifdef CONFIG_BOARD_NATIVE_SIM
		printf("[pwm] open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim — no PWM emul)\n",
		       (int)alp_last_error());
#else
		printf("[pwm] open failed: alp_last_error=%d\n", (int)alp_last_error());
#endif
		printf("[pwm] done\n");
		return 0;
	}

	/* Linear duty sweep up + down, one full cycle. */
	for (int dir = 0; dir < 2; dir++) {
		for (int i = 0; i <= STEPS; i++) {
			uint32_t     step  = (dir == 0) ? (uint32_t)i : (uint32_t)(STEPS - i);
			uint32_t     pulse = (PERIOD_NS / STEPS) * step;
			alp_status_t s     = alp_pwm_set_duty(led, pulse);
			if (s != ALP_OK) {
				printf("[pwm] set_duty(%u) -> %d\n", pulse, (int)s);
				goto out;
			}
			alp_delay_ms(STEP_DELAY_MS);
		}
	}

out:
	alp_pwm_close(led);
	printf("[pwm] done\n");
	return 0;
}
