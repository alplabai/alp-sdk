# audio-loopback

Per-peripheral example for `<alp/audio.h>`.  Reads PCM blocks
from the on-module PDM microphone, runs them through the ALP
DSP chain (DC-block in v0.2), and pushes them straight back
out over I²S.  Demonstrates the full `audio_in` / `audio_out`
lifecycle.

## What this shows

- Opening a PDM input (`ALP_E1M_PDM0`) and an I²S output
  (`ALP_E1M_I2S0`) by portable bus ID -- no SoM-specific
  device-tree node references in the application code.
- The `alp_audio_in_read` / `alp_audio_out_write` mirrored
  block-API.  Reading blocks of `FRAMES` samples at a time
  keeps latency bounded while letting the driver amortise
  the DMA transfers.
- Wrapper-managed DSP: the v0.2 DC-block runs inside
  `alp_audio_in_read` so the user loop stays trivial.  Real
  apps drop additional stages into the same chain via
  `<alp/dsp.h>` without touching the I/O path.
- Software volume control via `alp_audio_out_set_volume`
  (Q8 fraction of full-scale).

## Build

```bash
west build -b native_sim/native/64 examples/audio-loopback \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

On `native_sim` the example skips the open() calls (no PDM
or I²S devices are emulated) and exits with `[audio] done`,
which is what the twister scenario asserts on.

On a real E1M-AEN board with the EVK overlay enabled and
`CONFIG_AUDIO_DMIC=y` + `CONFIG_I2S=y`, the loop runs ~0.8 s
of audio (50 blocks × 256 frames @ 16 kHz) before tearing
down.

## Reference

- [`<alp/audio.h>`](../../include/alp/audio.h) -- the public surface
- [`<alp/dsp.h>`](../../include/alp/dsp.h) -- DSP chain definition
- [`i2s-tone`](../i2s-tone/) -- simpler I²S-only example
  (no mic path)
