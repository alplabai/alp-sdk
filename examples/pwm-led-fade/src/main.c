/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * pwm-led-fade — fade an LED on the portable PWM0 channel from
 * 0 % to 100 % and back, demonstrating the canonical alp_pwm_*
 * usage pattern.
 *
 * On native_sim there's no PWM emul controller, so alp_pwm_open
 * returns NULL with last_error = ALP_ERR_NOT_READY.  The example
 * prints the diagnostic and exits cleanly so the twister console
 * harness can assert it ran.
 *
 * On real silicon (EVK with E1M-AEN), the overlay's alp-pwm0
 * alias maps to a real pwm-leds child node and the LED breathes.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/pwm.h"

/* EVK_PWM_LED_GREEN is a board-macro from the generated routes header
 * (= E1M_PWM0); rebind it in board.yaml `pins:` to port to another board. */
#include "alp/boards/alp_e1m_evk_routes.h"

#define PERIOD_NS    1000000u   /* 1 kHz */
#define STEPS              50
#define STEP_DELAY_MS      20

int main(void) {
    printf("[pwm] open EVK_PWM_LED_GREEN (period=%u ns)\n", PERIOD_NS);

    alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
        .channel_id = EVK_PWM_LED_GREEN, /* = E1M_PWM0 */
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
            uint32_t step  = (dir == 0) ? (uint32_t)i : (uint32_t)(STEPS - i);
            uint32_t pulse = (PERIOD_NS / STEPS) * step;
            alp_status_t s = alp_pwm_set_duty(led, pulse);
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
