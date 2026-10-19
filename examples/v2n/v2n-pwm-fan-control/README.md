# v2n-pwm-fan-control

Drive a fan from `ALP_E1M_X_PWM0` on V2N.  Walks duty cycle through a
five-stop curve at a fixed 25 kHz PWM frequency (above audible) using the
portable `<alp/pwm.h>` surface, so the wave shape is observable on
a scope.

## What it shows

1. The canonical `alp_pwm_open` + `alp_pwm_set_duty` + `alp_pwm_close`
   usage pattern -- same call sequence as `examples/peripheral-io/pwm-led-fade/`,
   just with a fan curve instead of a sinusoidal sweep.
2. The portable `ALP_E1M_X_PWM0..PWM7` instance IDs from
   [`<alp/e1m_x_pinout.h>`](../../../include/alp/e1m_x_pinout.h) -- the
   application names the **E1M-X-standard** PWM channel, not the
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

To run the same fan curve on another SoM in the V2N/V2M family, edit
`board.yaml`:

```yaml
som:
  sku: E1M-V2M101      # E1M-V2N101 or E1M-V2M101; both carry a55_cluster + m33_sm
```

Moving to an AEN SoM such as E1M-AEN801 takes more than the SKU: the
`e1m-x-evk` preset only hosts the V2N/V2M families, and AEN has `m55_hp` /
`m55_he` instead of `m33_sm`, so `preset:` and the `cores:` block change
too, and the source switches to the `<alp/e1m_pinout.h>` `ALP_E1M_PWM0` ID.

The `alp_pwm_open(ALP_E1M_X_PWM0)` call dispatches to whichever
peripheral physically drives the pad on the active SoM (GD32 IO-MCU
bridge on V2N) -- the application never names a specific block.

## Scoping it

Attach a scope to whichever pad the active SoM's PWM0 resolves to
(consult `metadata/e1m_modules/<family>/` for the per-SoM mapping)
and you should see:

* **0 % duty**: line stays low.
* **30 % / 60 % / 85 % duty**: pulse train at 25 kHz, duty as scaled.
* **100 % duty**: line stays high.

The fan-curve dwell is 0.5-1.0 s per step so capturing a screenshot
across the full ramp is straightforward.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/v2n/v2n-pwm-fan-control`)

## See also

* [`<alp/pwm.h>`](../../../include/alp/pwm.h) -- portable PWM surface.
* [`<alp/e1m_x_pinout.h>`](../../../include/alp/e1m_x_pinout.h) -- the
  E1M-X-standard `ALP_E1M_X_PWM*` instance IDs.
* [`examples/peripheral-io/pwm-led-fade/`](../../peripheral-io/pwm-led-fade/) -- companion
  example using the same API on a single LED.
