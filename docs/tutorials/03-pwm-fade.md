<!-- Last verified: 2026-05-18 against slice-3b state. -->

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

The example uses 100 Hz so the fade is buttery-smooth on a slow
camera (and on the human eye).  Real fan-control firmware would
pick 25 kHz.

## Code path

```c
alp_pwm_t *pwm = alp_pwm_open(&(alp_pwm_config_t){
    .channel_id = 0u,
    .period_ns  = 10000000u,   /* 100 Hz */
});

for (;;) {
    /* duty is the active-level pulse width in ns; ramp it from 0 (off)
       up to the full period (fully on), then back down. */
    for (uint32_t duty = 0; duty <= 10000000u; duty += 100000u) {
        alp_pwm_set_duty(pwm, duty);
        k_msleep(20);
    }
    for (uint32_t duty = 10000000u; duty > 0; duty -= 100000u) {
        alp_pwm_set_duty(pwm, duty);
        k_msleep(20);
    }
}
```

## See also

* [`<alp/pwm.h>`](../../include/alp/pwm.h)
* [Tutorial 05](05-supervisor-mcu-bridge.md) for GD32-side PWM on V2N.
