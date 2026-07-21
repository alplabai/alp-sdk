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
shape -- the peripheral_id differs (`ALP_E1M_PDM0` vs `ALP_E1M_I2S0`).

```c
alp_audio_config_t cfg = {
    .peripheral_id    = ALP_E1M_PDM0,
    .sample_rate_hz   = 16000,
    .channels         = 1,
    .format           = ALP_AUDIO_FMT_S16_LE,
    .frames_per_block = 256,
};
alp_audio_in_t *mic = alp_audio_in_open(&cfg);

cfg.peripheral_id    = ALP_E1M_I2S0;
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
- The audio backend's built-in DC-block runs inside
  `alp_audio_in_read`, so the block is already DC-corrected
  when it lands in `pcm`.  For extra filtering, run the block
  through a standalone `<alp/dsp.h>` chain (see section 5).

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
tan build --board alif_e7_dk_rtss_he examples/audio/audio-loopback
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

`<alp/dsp.h>` exposes a **standalone** chain: build it from a
list of @ref alp_dsp_stage_t descriptors, then run captured
sample blocks through it.  The stage kinds are
`ALP_DSP_STAGE_FIR`, `ALP_DSP_STAGE_IIR`, `ALP_DSP_STAGE_WINDOW`,
and `ALP_DSP_STAGE_FFT`.  A DC-block + low-pass voice band is a
single-section biquad IIR (high-pass at ~80 Hz) followed by an
FIR low-pass -- both run on the captured samples:

```c
#include "alp/dsp.h"

/* coeffs are b0,b1,b2,a1,a2 per biquad section; lpf_taps is an FIR kernel */
alp_dsp_stage_t stages[] = {
    { .kind = ALP_DSP_STAGE_IIR,
      .u.iir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
                 .n_sections = 1u, .coeffs = hpf_biquad } },
    { .kind = ALP_DSP_STAGE_FIR,
      .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
                 .n_taps = 32u, .taps = lpf_taps } },
};
alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 2u);
```

The chain is applied to each captured block (it does not attach
to the stream itself): read with @ref alp_audio_in_read, then
filter the buffer in place before pushing it to the DAC.

```c
int16_t buf[256];
size_t  got = 0u, out_n = 0u;
alp_audio_in_read(mic, buf, 256u, &got, 100u);
alp_dsp_chain_apply_samples(chain, buf, got, buf, 256u, &out_n);
alp_audio_out_write(spk, buf, out_n, NULL, 100u);
```

CMSIS-DSP backend handles the FIR + biquad math when
`ALP_HAS_CMSIS_DSP=1`; portable-C fallback always available.

See [ADR 0007](../adr/0007-wave2-dsp-pipeline-design.md) for
why the DSP surface is shaped this way.

## 6. Yocto-side variant

On Yocto the same `<alp/audio.h>` API maps to ALSA's
`snd_pcm_*` via `src/backends/audio/yocto_drv.c`.  Differences:

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
