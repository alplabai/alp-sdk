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
 * resolves to E1M_PWM0 on E1M EVK (RGB LED green via PWM0) and
 * E1M_X_PWM7 on E1M-X EVK (RGB LED green via PWM7).  On real
 * silicon the LED breathes at 1 kHz across a linear duty sweep.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/pwm.h"

/* BOARD_PWM_LED_GREEN is the portable alias from <alp/board.h>
 * (E1M_PWM0 on E1M EVK; E1M_X_PWM7 on E1M-X EVK). */
#include "alp/board.h"

#define PERIOD_NS 1000000u /* 1 kHz */
#define STEPS 50
#define STEP_DELAY_MS 20

int main(void)
{
    printf("[pwm] open BOARD_PWM_LED_GREEN (period=%u ns)\n", PERIOD_NS);

    alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
        .channel_id = BOARD_PWM_LED_GREEN, /* E1M_PWM0 on E1M EVK; E1M_X_PWM7 on E1M-X EVK */
        .period_ns  = PERIOD_NS,
        .polarity   = ALP_PWM_POLARITY_NORMAL,
    });
    if (led == NULL) {
        printf("[pwm] open failed: alp_last_error=%d "
               "(expected NOT_READY = -2 on native_sim — no PWM emul)\n",
               (int)alp_last_error());
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
            k_msleep(STEP_DELAY_MS);
        }
    }

out:
    alp_pwm_close(led);
    printf("[pwm] done\n");
    return 0;
}
