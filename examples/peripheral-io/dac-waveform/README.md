# dac-waveform

Generate a sine wave on `BOARD_DAC0`.

Walks a 32-point sine lookup table at a configurable sample rate,
writing each sample to the DAC via `alp_dac_write_mv`.  The
staircase output approximates a sine at the requested frequency
(32 samples per cycle -> 32-step staircase).  Pop a DSO probe on
the `ANA_OUT0` pad to see it.

## How DAC0 is provided on each EVK

This example runs on BOTH EVKs.  `BOARD_DAC0` (from `<alp/board.h>`)
resolves to the selected board's DAC0 channel -- the source code
never names a form-factor-specific instance ID:

* **E1M EVK** -- the base E1M (35x35 mm) SoM carries the Alif
  Ensemble, whose native 2-channel DAC drives `ANA_OUT0` / `ANA_OUT1`
  directly.  `BOARD_DAC0` resolves to `ALP_E1M_DAC0`.
* **E1M-X EVK** -- the E1M-X (45x65 mm, V2N family) SoM carries the
  Renesas RZ/V2N.  The RZ/V2N SoC itself has no DAC peripheral
  (`ALP_SOC_DAC_COUNT 0`), so the SoM provides DAC0/DAC1 via the
  on-module GD32G553 supervisor bridge: the SDK routes `alp_dac_*`
  calls to the GD32's PA4 (DAC0) / PA6 (DAC1) pads
  (per [`metadata/e1m_modules/E1M-V2N101.yaml`](../../../metadata/e1m_modules/E1M-V2N101.yaml)
  `pad_routes:` block).  `BOARD_DAC0` resolves to the V2N SoM's
  GD32-bridged DAC0 channel.

Either way the example sees a working DAC0 -- neither EVK lacks one.
The GD32 dispatch on E1M-X is transparent to the `<alp/dac.h>` DAC API.

The shipped `board.yaml` defaults to the **E1M EVK** (Alif Ensemble E8,
`som.sku: E1M-AEN801`) -- the native DAC12 path.  Switch to the V2N
GD32-bridged path with `preset: e1m-x-evk` + a V2N SKU (both are listed
under `supported_boards:`).

## What this shows

* `alp_dac_open()` with a board-neutral channel ID
  (`BOARD_DAC0`, resolved per board via `<alp/board.h>`).
* `alp_dac_write_mv()` -- one mV setpoint write per sample.
* Sample-rate control via `alp_delay_us` between samples.
* Integer-only Q15 LUT scaling (no FPU required on Cortex-M33).
* Clean shutdown: park at mid-rail before close.

## Build

```bash
# Standalone, native_sim (compiles + open returns NOT_READY -- no
# DAC controller on the host build):
west build -b native_sim/native/64 examples/peripheral-io/dac-waveform \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real V2N silicon:
west build -b alp_e1m_v2n101_m33_sm examples/peripheral-io/dac-waveform
west flash
```

## Expected output

native_sim (no DAC):

```
[dac] open BOARD_DAC0 (initial 1650 mV)
[dac] open failed: alp_last_error=-2 (NOT_READY on native_sim; DAC backend not ready on real hardware)
[dac] done
```

V2N hardware (GD32 supervisor ready):

```
[dac] open BOARD_DAC0 (initial 1650 mV)
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
* **DAC1 instead of DAC0.**  Change `BOARD_DAC0` ->
  `BOARD_DAC1`.  The board routes that to ANA_OUT1.

## Verifying with a DMM (no scope)

A DMM in AC RMS mode will read:

  V_rms = SINE_AMPLITUDE_MV / sqrt(2) = 1650 / 1.414 ≈ 1167 mV

A DMM in DC mode will read SINE_DC_OFFSET_MV (1650 mV).

If neither value matches, your DAC isn't reaching the pad --
suspect board routing or supervisor-not-ready
(`open` returning NULL with last_error = `ALP_ERR_NOT_READY`).

## Reference

- [`<alp/dac.h>`](../../../include/alp/dac.h) -- DAC API lives here.
- [`<alp/board.h>`](../../../include/alp/board.h)
  -- `BOARD_DAC0` / `BOARD_DAC1` board-neutral aliases.
- [`metadata/e1m_modules/E1M-V2N101.yaml`](../../../metadata/e1m_modules/E1M-V2N101.yaml)
  -- V2N `pad_routes:` for the GD32-bridged DAC0 / DAC1 (E1M-X).
- [`docs/architecture.md`](../../../docs/architecture.md)
  -- GD32 supervisor bridge architecture.
