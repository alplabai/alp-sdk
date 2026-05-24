# audio-noise-suppression  ![UNTESTED](https://img.shields.io/badge/status-UNTESTED-yellow)

Real-time noise suppression demo on the V2N (and V2H by SKU
flip).  Target market: USB-C headsets, true-wireless hearables,
desktop conferencing rigs -- anywhere you want cloud-grade
denoising at sub-perceptual latency without shipping audio
off-device.

## Pipeline shape

```
TAS2563 mic  ──▶  <alp/audio.h>  ──▶  <alp/dsp.h>     ──▶  <alp/inference.h>  ──▶  <alp/audio.h>  ──▶  TAS2563 spk
(I2S0 RX,        240 fr / 10 ms      Hann + 256-pt        RNNoise-style int8     I2S0 TX
 24 kHz S16)     blocks               FFT -> mag bins      CNN -> gain mask
```

The spectral work routes through whichever backend the SoM has:

- **V2N M33 supervisor**: there's no native math accelerator on
  the RZ/V2N M33 (see project memory
  `v2n_no_dedicated_math_accelerator`); `<alp/dsp.h>` offloads
  the FFT/FAC to the GD32G553 bridge over the existing SPI/I²C
  command frame.
- **V2N A55 (Yocto)**: CMSIS-DSP on Neon directly -- the Neon
  path beats a bridge round-trip for the A55.
- **V2H**: same shape as V2N once the V2H DSP path lands.
- **native_sim**: portable C reference kernels; framing only.

Inference dispatches via `<alp/inference.h>` AUTO:

- **V2N-M1** (E1M-V2M101) -> DEEPX DX-M1 NPU, ~3 ms invoke.
- **V2N** (E1M-V2N101)    -> CPU TFLM reference kernels.
- **native_sim**           -> CPU TFLM reference kernels.

## Latency budget @ 24 kHz, 240-frame blocks (10 ms each)

| stage                               | budget   |
|-------------------------------------|----------|
| Mic -> I2S RX DMA                   | ~2.0 ms  |
| `<alp/dsp.h>` FFT pipe (GD32 bridge)| ~1.5 ms  |
| `<alp/inference.h>` invoke (DX-M1)  | ~3.0 ms  |
| Mask apply + IFFT (TODO v0.6)       | ~1.5 ms  |
| I2S TX DMA push                     | ~2.0 ms  |
| **total**                           | ~10.0 ms |

The block period **is** the latency budget -- DMA ping-pong lets
each step touch the previous block while the I2S peripheral
grabs the next one.

## What's stubbed in v0.5

- Real RNNoise-style model bytes (`s_model[]` is a 1-byte stub).
- DSP-chain weights / FFT bin-count / hop length / mask-apply
  weights -- all TODO(v0.6).
- IFFT + overlap-add synthesis path -- v0.5 passthroughs the
  mic block to the speaker so HiL still hears the loop.

## Reference

- [`<alp/audio.h>`](../../../include/alp/audio.h)
- [`<alp/dsp.h>`](../../../include/alp/dsp.h)
- [`<alp/inference.h>`](../../../include/alp/inference.h)
- [`audio-loopback`](../audio-loopback/) -- simpler mic -> speaker (no DSP / no AI)
- [`audio-wake-word`](../audio-wake-word/) -- always-on KWS (AEN path, no speaker out)
