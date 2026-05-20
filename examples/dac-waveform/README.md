# dac-waveform

Generate a sine wave on `E1M_X_DAC0`.

Walks a 32-point sine lookup table at a configurable sample rate,
writing each sample to the DAC via `alp_dac_write_mv`.  The
staircase output approximates a sine at the requested frequency
(32 samples per cycle -> 32-step staircase).  Pop a DSO probe on
the `ANA_OUT0` pad to see it.

## Why E1M-X (V2N family)?

The base E1M (35x35 mm) form factor does NOT route a DAC output
pad.  Only the E1M-X (45x65 mm, V2N family) form factor exposes
`ANA_OUT0` / `ANA_OUT1`.

On V2N the Renesas RZ/V2N SoC has no DAC peripheral at all -- the
SDK routes `alp_dac_*` calls through the on-module GD32G553
supervisor bridge to the GD32's PA4 (DAC0) / PA6 (DAC1) pads
(per [`metadata/e1m_modules/E1M-V2N101.yaml`](../../metadata/e1m_modules/E1M-V2N101.yaml)
`pad_routes:` block).  Customer code doesn't see the dispatch --
it's transparent to the `<alp/adc.h>` DAC API.

## What this shows

* `alp_dac_open()` with E1M-X form-factor instance IDs
  (`E1M_X_DAC0`).
* `alp_dac_write_mv()` -- one mV setpoint write per sample.
* Sample-rate control via `alp_delay_us` between samples.
* Integer-only Q15 LUT scaling (no FPU required on Cortex-M33).
* Clean shutdown: park at mid-rail before close.

## Build

```bash
# Standalone, native_sim (compiles + open returns NOT_READY -- no
# DAC controller on the host build):
west build -b native_sim/native/64 examples/dac-waveform \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real V2N silicon:
west build -b alp_e1m_v2n101_m33_sm examples/dac-waveform
west flash
```

## Expected output

native_sim (no DAC):

```
[dac] open E1M_X_DAC0 (initial 1650 mV)
[dac] open failed: alp_last_error=-2 (NOT_READY on native_sim; supervisor not ready on real V2N)
[dac] done
```

V2N hardware (GD32 supervisor ready):

```
[dac] open E1M_X_DAC0 (initial 1650 mV)
[dac] generating sine: freq=100 Hz, mean=1650 mV, ampl=1650 mV
[dac] cycle 0: peak=3299 mV trough=0 mV
[dac] cycle 1: peak=3299 mV trough=0 mV
[dac] cycle 2: peak=3299 mV trough=0 mV
[dac] done
```

On a DSO probe attached to `ANA_OUT0`:

* 100 Hz sine wave, 0..3.3 V swing.
* 12-bit resolution (~0.8 mV / LSB on the 3.3 V reference) --
  the staircase is barely perceptible without a high-bandwidth
  scope.

## Customising

* **Different frequency.**  Bump `SINE_FREQ_HZ` to 1000 (audio
  test tone) or drop to 10 (DC-ish trickle).  The sample-rate
  math automatically adjusts the inter-sample delay.
* **Different amplitude.**  Drop `SINE_AMPLITUDE_MV` to 500
  (smaller signal, less clipping risk) or raise to match the
  full rail.  Keep `SINE_DC_OFFSET_MV + SINE_AMPLITUDE_MV` <=
  3300 mV to avoid clipping at the top.
* **Different waveform.**  Replace `SINE_LUT_Q15` with a
  triangle, sawtooth, or arbitrary user-defined sequence.
  Keep entries in Q15 (signed 16-bit centred on 0) so the
  `lut_to_mv` scaler stays unchanged.
* **DAC1 instead of DAC0.**  Change `E1M_X_DAC0` ->
  `E1M_X_DAC1`.  The board routes that to ANA_OUT1.

## Verifying with a DMM (no scope)

A DMM in AC RMS mode will read:

  V_rms = SINE_AMPLITUDE_MV / sqrt(2) = 1650 / 1.414 ≈ 1167 mV

A DMM in DC mode will read SINE_DC_OFFSET_MV (1650 mV).

If neither value matches, your DAC isn't reaching the pad --
suspect board routing or supervisor-not-ready
(`open` returning NULL with last_error = `ALP_ERR_NOT_READY`).

## Reference

- [`<alp/adc.h>`](../../include/alp/adc.h) -- DAC API lives here.
- [`<alp/e1m_x_pinout.h>`](../../include/alp/e1m_x_pinout.h)
  -- E1M-X instance IDs.
- [`metadata/e1m_modules/E1M-V2N101.yaml`](../../metadata/e1m_modules/E1M-V2N101.yaml)
  -- V2N `pad_routes:` for `E1M_X_DAC0` / `E1M_X_DAC1`.
- [`docs/architecture.md`](../../docs/architecture.md)
  -- GD32 supervisor bridge architecture.
