<!-- Last verified: 2026-08-05 against alp-sdk (examples/peripheral-io/pwm-led-fade/src/main.c
     and BOARD_PWM_LED_GREEN's ALP_E1M_PWM3 / ALP_E1M_X_PWM7 resolution
     re-checked against include/alp/boards/alp_e1m_evk_routes.h and
     alp_e1m_x_evk_routes.h; content unchanged, still accurate). -->

# 03 -- PWM fade

Walks `examples/peripheral-io/pwm-led-fade/`.  A PWM channel ramps its duty cycle
from 0 to 100 % and back -- the LED visibly fades up + down.  This
is the "I can control analog brightness" smoke test for the PWM
wrapper.

## What it teaches

* How to open an `alp_pwm_t` with an explicit period (frequency).
* That `duty_ns = period_ns` is "permanently on" and `duty_ns = 0`
  is "permanently off"; values in between produce the board.
* When to use the host-SoC PWM vs the GD32 supervisor's PWM (V2N
  only, see Tutorial 05).

## Period selection

```
1 kHz   = 1_000_000 ns period  -- visible flicker for low duty
10 kHz  = 100_000 ns  period   -- still in audible range
25 kHz  = 40_000 ns   period   -- above audible (fan control)
100 Hz  = 10_000_000 ns period -- LED dimming with no flicker
```

The example uses 1 kHz (`PERIOD_NS = 1000000`) -- fast enough that
the eye sees a smooth brightness ramp rather than a flicker, matching
the "LED breathes" behaviour described in the example's own header
comment.  Real fan-control firmware would pick 25 kHz instead.

## Code path

```c
#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/board.h"      /* BOARD_PWM_LED_GREEN cross-EVK alias */

#define PERIOD_NS     1000000u   /* 1 kHz */
#define STEPS         50
#define STEP_DELAY_MS 20

int main(void) {
    (void)alp_init();

    alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
        .channel_id = BOARD_PWM_LED_GREEN,   /* E1M EVK: ALP_E1M_PWM3 */
        .period_ns  = PERIOD_NS,
        .polarity   = ALP_PWM_POLARITY_NORMAL,
    });
    if (led == NULL) return 1;   /* e.g. ALP_ERR_NOT_READY on native_sim -- no PWM emul */

    /* Linear duty sweep up, then back down -- one full fade cycle. */
    for (int dir = 0; dir < 2; dir++) {
        for (int i = 0; i <= STEPS; i++) {
            uint32_t step  = (dir == 0) ? (uint32_t)i : (uint32_t)(STEPS - i);
            uint32_t pulse = (PERIOD_NS / STEPS) * step;
            alp_pwm_set_duty(led, pulse);
            alp_delay_ms(STEP_DELAY_MS);
        }
    }

    alp_pwm_close(led);
}
```

`BOARD_PWM_LED_GREEN` (from `<alp/board.h>`) resolves to
`ALP_E1M_PWM3` on the E1M EVK and `ALP_E1M_X_PWM7` on the E1M-X
EVK, so the same source drives either board's RGB-green channel
without changes.  `native_sim` has no PWM emulation controller, so
`alp_pwm_open` returns NULL there with `alp_last_error() ==
ALP_ERR_NOT_READY` -- the example checks for that and exits cleanly
rather than looping forever.

## See also

* [`<alp/pwm.h>`](../../include/alp/pwm.h)
* [`<alp/board.h>`](../../include/alp/board.h)
* [Tutorial 05](05-supervisor-mcu-bridge.md) for GD32-side PWM on V2N.
