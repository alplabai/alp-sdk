# v2n-pwm-fan-control

Drive a fan from `E1M_PWM0` on V2N.  Walks duty cycle through a
five-stop curve at a fixed 25 kHz board (above audible) using the
portable `<alp/pwm.h>` surface, so the wave shape is observable on
a scope.

## What it shows

1. The canonical `alp_pwm_open` + `alp_pwm_set_duty` + `alp_pwm_close`
   usage pattern -- same call sequence as `examples/peripheral-io/pwm-led-fade/`,
   just with a fan curve instead of a sinusoidal sweep.
2. The portable `E1M_PWM0..PWM7` instance IDs from
   [`<alp/e1m_pinout.h>`](../../../include/alp/e1m_pinout.h) -- the
   application names the **E1M-standard** PWM channel, not the
   underlying SoC peripheral.  The SDK resolves which silicon
   block actually drives the pad.
3. A fan-curve table that an application would normally drive from
   a thermistor reading; here we cycle through the stops on a fixed
   dwell so the duty staircase is visible.
4. The integer-only `duty_ns = period_ns * duty_pct_x10 / 1000`
   conversion idiom -- avoids floats in a controller loop while
   keeping one decimal of resolution.

## Why a V2N case study?

V2N modules carry the Renesas RZ/V2N (~5-8 W typical) plus a DEEPX
DX-M1 NPU on the M1 SKUs (~10-15 W under load), so a board-driven
fan is a normal accessory.  The example is named for the **scenario**;
the **code** is fully portable.

To run the same fan curve on a different SoM, edit `board.yaml`:

```yaml
som:
  sku: E1M-AEN701      # or E1M-V2M101, E1M-N93xxx, ...
```

The `alp_pwm_open(E1M_PWM0)` call dispatches to whichever
peripheral physically drives the pad on the active SoM (Alif GPT on
AEN, GD32 IO-MCU bridge on V2N, NXP TPU on i.MX 93) -- the
application never names a specific block.

## Scoping it

Attach a scope to whichever pad the active SoM's PWM0 resolves to
(consult `metadata/e1m_modules/<family>/` for the per-SoM mapping)
and you should see:

* **0 % duty**: line stays low.
* **30 % / 60 % / 85 % duty**: pulse train at 25 kHz, duty as scaled.
* **100 % duty**: line stays high.

The fan-curve dwell is 0.5-1.0 s per step so capturing a screenshot
across the full ramp is straightforward.

## See also

* [`<alp/pwm.h>`](../../../include/alp/pwm.h) -- portable PWM surface.
* [`<alp/e1m_pinout.h>`](../../../include/alp/e1m_pinout.h) -- the
  E1M-standard `E1M_PWM*` instance IDs.
* [`examples/peripheral-io/pwm-led-fade/`](../pwm-led-fade/) -- companion
  example using the same API on a single LED.
