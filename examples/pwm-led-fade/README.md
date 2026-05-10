# pwm-led-fade

Per-peripheral example for `<alp/pwm.h>`.  Fades an LED on PWM
channel `ALP_E1M_PWM0` from 0 % to 100 % and back, demonstrating
the canonical open / set-duty / close pattern.

## What this shows

- Resolving a portable PWM channel ID (`ALP_E1M_PWM0`) into a
  driver handle via `alp_pwm_open`.
- Updating the duty cycle in a tick loop with `alp_pwm_set_duty`.
- Reading `alp_last_error()` to diagnose `*_open` failures —
  e.g. when the build's devicetree has no `alp-pwm0` alias.

## Build (standalone, native_sim)

```bash
west build -b native_sim/native/64 examples/pwm-led-fade \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Build (on real silicon, EVK with E1M-AEN)

The example's overlay points the `alp-pwm0` alias at the EVK's
user LED.  Once `alplabai/alp-zephyr-modules` ships the EVK board
file, build with:

```bash
west build -b alp_e1m_evk_aen examples/pwm-led-fade \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west flash
```

## Reference

- [`<alp/pwm.h>`](../../include/alp/pwm.h)
- [ADR 0003 — peripheral coverage](../../docs/adr/0003-peripheral-coverage.md)
