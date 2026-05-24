<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 14: Audio loopback (PDM → DSP → I²S)

**Target audience:** developers building audio-aware firmware
on AEN-Zephyr (where PDM mic + I²S DAC are both routed) or
Yocto (V2N / N93 with ALSA backend).

**Prerequisites:** Tutorial [01](01-first-build.md) completed.
A working AEN EVK with a PDM mic + I²S DAC populated (or a Yocto
target with ALSA-backed audio hardware).

**Outcome:** a running firmware that captures from the PDM mic,
runs a DC-block DSP stage, and pushes the audio out over I²S
with software volume control.  Understand `<alp/audio.h>`'s
mirrored in/out API + the DSP-chain hook.

**Time:** 20 minutes (after the EVK is on the bench).

---

## What you'll build

The `examples/audio/audio-loopback/` reference app, walked stage by
stage.  Loops the on-board PDM mic into the I²S DAC with the
SDK's v0.2 DC-block DSP stage in the middle.  Real-silicon
runs ~0.8 s of audio (50 blocks × 256 frames @ 16 kHz) then
exits cleanly.

## 1. Open mic + DAC

The `<alp/audio.h>` surface mirrors input and output through
two distinct handle types: `alp_audio_in_t` and
`alp_audio_out_t`.  Both take a config struct with the same
shape -- the peripheral_id differs (`E1M_PDM0` vs `E1M_I2S0`).

```c
alp_audio_config_t cfg = {
    .peripheral_id    = E1M_PDM0,
    .sample_rate_hz   = 16000,
    .channels         = 1,
    .format           = ALP_AUDIO_FMT_S16_LE,
    .frames_per_block = 256,
};
alp_audio_in_t *mic = alp_audio_in_open(&cfg);

cfg.peripheral_id    = E1M_I2S0;
alp_audio_out_t *spk = alp_audio_out_open(&cfg);
```

Both calls return NULL on a SoM that doesn't route the
matching peripheral.  Check `alp_last_error()` to distinguish
"not routed" from "config rejected".

## 2. Block loop

```c
const int BLOCKS = 50;
static int16_t pcm[256];  // frames_per_block × channels

alp_audio_in_start(mic);
alp_audio_out_start(spk);

for (int b = 0; b < BLOCKS; ++b) {
    size_t got = 0;
    if (alp_audio_in_read(mic, pcm, 256, &got, 1000) != ALP_OK) break;

    size_t pushed = 0;
    alp_audio_out_write(spk, pcm, got, &pushed, 1000);
}
```

What's happening:

- `alp_audio_in_read` blocks up to 1000 ms waiting for a full
  block (256 frames at 16 kHz = 16 ms of audio).  Under-run
  recovery is the backend's responsibility.
- `alp_audio_out_write` queues the block for DMA-driven
  playback.  Returns OK as soon as the block is queued; the
  DMA does the actual push.
- DSP runs inside `alp_audio_in_read` -- the DC-block stage
  is applied to the block before it lands in `pcm`.  See
  `<alp/dsp.h>` for the chain config API; default chain is
  DC-block only.

## 3. Software volume

```c
alp_audio_out_set_volume(spk, 0x9A);  // ~60% of full-scale, Q8
```

Q8 fraction of `0x100` = full-scale.  `0x9A` ≈ 154/256 ≈ 60 %.
Real apps would also drive the codec's analog gain pin via
`alp_gpio_*` for the full dynamic range.

## 4. Build + run

### native_sim

```bash
west build -b native_sim/native/64 examples/audio/audio-loopback
west build -t run
```

Expected output:

```
[audio] audio-loopback v0.2 reference -- mic -> DSP -> DAC
[audio]   alp_audio_in_open               skip (no DMIC, last_err=ALP_ERR_NOSUPPORT)
[audio] done
```

native_sim skips because neither the PDM mic nor the I²S DAC
have simulator backends; the example exits cleanly via the
NOSUPPORT fallback.

### Real silicon (AEN-Zephyr)

```bash
west alp-build -b alif_e7_dk_rtss_he examples/audio/audio-loopback
west flash
```

Expected output:

```
[audio] audio-loopback v0.2 reference -- mic -> DSP -> DAC
[audio]   alp_audio_in_open(PDM0)         ok
[audio]   alp_audio_out_open(I2S0)        ok
[audio]   streaming 50 blocks of 256 frames @ 16 kHz
[audio]   loopback complete
[audio] done
```

Tap the mic; you'll hear it on the I²S DAC's connected
speaker.

## 5. Adding DSP stages

To swap the default DC-block for a multi-stage chain (DC-block
+ low-pass filter + AGC):

```c
#include "alp/dsp.h"

alp_dsp_stage_t stages[] = {
    { .kind = ALP_DSP_STAGE_DC_BLOCK },
    { .kind = ALP_DSP_STAGE_FIR,
      .fir  = { .coeffs = lpf_taps, .n_taps = 32 } },
    { .kind = ALP_DSP_STAGE_AGC,
      .agc  = { .target_dbfs = -12, .attack_ms = 50 } },
};
alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 3);

alp_audio_in_attach_chain(mic, chain);
```

The chain runs inside the `alp_audio_in_read` block path.
CMSIS-DSP backend handles the FIR + biquad math when
`ALP_HAS_CMSIS_DSP=1`; portable-C fallback always available.

See [ADR 0007](../adr/0007-wave2-dsp-pipeline-design.md) for
why the DSP surface is shaped this way.

## 6. Yocto-side variant

On Yocto the same `<alp/audio.h>` API maps to ALSA's
`snd_pcm_*` via `src/yocto/audio_yocto.c`.  Differences:

- `peripheral_id == 0` → ALSA `"default"` device.
- `peripheral_id == N` (N > 0) → ALSA `"hw:N-1,0"` device.
- Sample-rate / format conversion: ALSA handles it via plug;
  the SDK passes through.

`board.yaml`:

```yaml
cores:
  a55_cluster:
    app: ./linux                  # os: omitted -- A-cores default to yocto per topology
    peripherals: [i2s]            # i2s carries the audio data path; <alp/audio.h> is
                                  # the higher-level surface composed on top
```

Build + run as a normal Yocto userspace binary.

## 7. Troubleshooting

- **`alp_audio_in_open` returns NULL with NOSUPPORT** -- the
  SoM's PDM mic isn't routed.  Confirm via the SKU's preset
  (`metadata/e1m_modules/E1M-<MPN>.yaml` peripherals block).
- **Loopback works but audio is silent** -- volume too low or
  the I²S DAC's amp not enabled.  Some boards gate the amp
  via a GPIO (see the SKU's `board.populated` list for the
  TAS2563 amp driver opt-in).
- **Audio crackles / drops blocks** -- buffer underrun.
  Increase `frames_per_block` to 512 to absorb more jitter;
  check the kernel log for `xrun` events on Yocto.

## See also

- [`<alp/audio.h>`](../../include/alp/audio.h) -- the public API.
- [`<alp/dsp.h>`](../../include/alp/dsp.h) -- the DSP chain
  surface.
- [`examples/audio/audio-loopback/`](../../examples/audio/audio-loopback/)
  -- the reference app this tutorial walks.
- [ADR 0007](../adr/0007-wave2-dsp-pipeline-design.md) --
  rationale for the DSP-chain shape.
