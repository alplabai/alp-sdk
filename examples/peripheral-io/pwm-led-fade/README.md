# pwm-led-fade

Per-peripheral example for `<alp/pwm.h>`.  Fades an LED on PWM
channel `ALP_E1M_PWM3` from 0 % to 100 % and back, demonstrating
the canonical open / set-duty / close pattern.

## What this shows

- Resolving a portable PWM channel ID (`ALP_E1M_PWM3`) into a
  driver handle via `alp_pwm_open`.
- Updating the duty cycle in a tick loop with `alp_pwm_set_duty`.
- Reading `alp_last_error()` to diagnose `*_open` failures —
  e.g. when the build's devicetree has no `alp-pwm3` alias.

## Build (standalone, native_sim)

```bash
west build -b native_sim/native/64 examples/peripheral-io/pwm-led-fade \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Build (on real silicon, EVK with E1M-AEN)

`board.yaml`'s declarative `pins:`/`peripherals:` route alone emits no
devicetree alias -- this example ships its own board-qualified overlay,
[`boards/alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay`](boards/alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay),
which Zephyr auto-applies by board-target filename.  It points the
`alp-pwm3` alias at the EVK's green LED channel (`ALP_E1M_PWM3` / pad
P2_4, via UTIMER10) -- see the overlay's header comment for the full
wiring.  The shared EVK board tree
([`zephyr/boards/alp/e1m_aen801_m55_hp/`](../../../zephyr/boards/alp/e1m_aen801_m55_hp/))
carries only the base boot + console wiring, not this alias.  Build with:

```bash
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/peripheral-io/pwm-led-fade \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west flash
```

## Reference

- [`<alp/pwm.h>`](../../../include/alp/pwm.h)
- [ADR 0003 — peripheral coverage](../../../docs/adr/0003-peripheral-coverage.md)
