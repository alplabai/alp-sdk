# v2n-pwm-fan-control

Drive a fan from a GD32-side PWM channel on V2N.  Walks duty cycle
through a five-stop curve at a fixed 25 kHz carrier (above audible),
calling `gd32g553_pwm_set` once per setpoint so the wave shape is
observable on a scope.

## What it shows

1. Opening the **SPI fast path** to the GD32 bridge with
   `alp_spi_open` + `gd32g553_init`.
2. Calling `gd32g553_pwm_set(channel, period_ns, duty_ns)` to
   program a PWM channel that physically lives on the GD32, not on
   the Renesas SoC.
3. A fan-curve table that an application would normally drive from a
   thermistor reading; here we cycle through the stops on a fixed
   dwell so the duty staircase is visible.
4. The integer-only `duty_ns = period_ns * duty_pct_x10 / 1000`
   conversion idiom -- avoids floats in a controller loop while
   keeping one decimal of resolution.

## Why a GD32-side PWM?

After the 2026-05-11 V2N schematic revision two of the eight E1M
PWM channels are wired to GD32 pads instead of the Renesas SoC
pinmux.  Driving them happens via the bridge -- the host calls
`gd32g553_pwm_set`, the GD32 firmware programs its own timer +
GTIOC, and the carrier sees a clean PWM waveform.  See
[`docs/gd32-bridge-protocol.md`](../../docs/gd32-bridge-protocol.md)
§3.2.

## Scoping it

Attach a scope to whichever GD32 pad the firmware's
`pwm_channel_map[0]` resolves to and you should see:

* **0% duty**: line stays low.
* **30% / 60% / 85% duty**: pulse train at 25 kHz, duty as scaled.
* **100% duty**: line stays high.

The fan-curve dwell is 0.5-1.0 s per step so capturing a screenshot
across the full ramp is straightforward.

## See also

* [`<alp/chips/gd32g553.h>`](../../include/alp/chips/gd32g553.h) --
  host driver API.
* [`docs/gd32-bridge-protocol.md`](../../docs/gd32-bridge-protocol.md)
  §3.2 -- PWM opcode wire format + channel-mapping convention.
