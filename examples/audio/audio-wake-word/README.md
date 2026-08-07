# audio-wake-word  ![status: UNTESTED](https://img.shields.io/badge/status-%5BUNTESTED%5D-orange)

Always-on "Hey Alp" keyword spotting on the **E1M-AEN** family's
low-power AI subsystem. Targets the Cortex-M55 HE ("High
Efficiency") core at ~50 MHz with the on-die Ethos-U55 NPU
bursting the convolutions on demand.

> Builds clean on `native_sim/native/64`. The bench-verified pieces
> this example depends on belong to two vendor-direct examples, not
> to this example's own SDK backends
> (`src/backends/audio/zephyr_drv.c`,
> `src/backends/inference/ethos_u_aen.cpp`) -- neither has run on
> silicon. PDM mic capture ("PDM mics -- Live varying PCM = real
> audio",
> [`docs/aen-bench-bringup.md`](../../../docs/aen-bench-bringup.md))
> ran through `examples/aen/aen-pdm-mic-alif`, which drives the
> Zephyr `dmic_configure`/`dmic_trigger`/`dmic_read` API directly,
> bypassing `<alp/audio.h>`. NPU inference (person_detect /
> keyword_scrambled, same doc) ran through
> `examples/aen/aen-npu-inference-person-mram`'s own
> `ethosu_utils/inference_process.cpp` calling `ethosu_invoke`
> directly, bypassing `<alp/inference.h>` -- and on the Ethos-U85 (a
> different NPU/core); the Ethos-U55-HE this example targets has only
> had an ID readback on silicon (`0x10104201`, same doc). Per the
> "`<alp/inference.h>` Ethos-U on AEN" row of
> [`docs/verification-status.md`](../../../docs/verification-status.md)
> (generated from [`docs/test-plan.md`](../../../docs/test-plan.md)),
> the SDK's own per-NPU TFLM driver gates
> (`CONFIG_ALP_TFLM_ETHOS_U85/U65/U55`) are Kconfig-reachable, but no
> Vela-compiled model has been dispatched THROUGH this portable
> backend yet -- only through the vendor-direct examples named above,
> which bypass it. This example's own MFCC feature extraction and wake-word decode
> (`extract_mfcc`/`decode_wake` in `src/main.c`) are still stubs, and
> the model bytes (`s_model[]`) are still a placeholder.

## The AEN pitch (vs V2N)

The AEN family's headline differentiator is its always-on AI
complex. The M55 HE core stays awake 24/7 at low clock running
the keyword-spotter front-end; each 50 ms inference window
dispatches the conv layers to the Ethos-U55 NPU and parks the
M55 HE in WFI for the rest of the window. Duty-cycle math:
**average system power < 1 mW** for continuous listening.

E1M-V2N's A55 cluster + M33 lockstep doesn't have an equivalent
always-on AI pairing — the smallest cores there are not bonded
to an NPU, so KWS on V2N either runs on the M33 in software
(~20x the power) or burns the A55 (~100x). This demo is the
SDK's "why pick AEN over V2N" exhibit.

## Wake-up path

```
WIC (mic activity)  ──▶  M55 HE wakes from STOP
                          │
                          ▼
                    mic_enable + 50 ms PDM read
                          │
                          ▼
                  MFCC features (Helium MVE on M55 HE)
                          │
                          ▼
                  CNN inference (Ethos-U55 burst)
                          │
                  ┌───────┴───────┐
                  │ no match      │ match
                  ▼               ▼
              k_sleep        M55 HP wake-up
                             (ASR / cloud / heavy)
```

## Build

```
west build -b ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp examples/audio/audio-wake-word
west flash
```

On `native_sim` the PDM mic + Ethos-U paths NOSUPPORT-stub; the
loop still exercises the inference dispatch + post-process
plumbing and prints `[wake] done`. TODO(v0.6): drop the
Vela-compiled `hey_alp_vela.tflite` into `models/` and replace
the `s_model[]` placeholder.
