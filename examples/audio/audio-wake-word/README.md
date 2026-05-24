# audio-wake-word

> ![status: UNTESTED](https://img.shields.io/badge/status-%5BUNTESTED%5D-orange)
> v0.5 paper-correct. Builds clean on `native_sim/native/64`; real
> PDM mic capture + Ethos-U55 dispatch + Vela-compiled model land
> in v0.6 AEN HiL.

Always-on "Hey Alp" keyword spotting on the **E1M-AEN** family's
low-power AI subsystem. Targets the Cortex-M55 HE ("High
Efficiency") core at ~50 MHz with the on-die Ethos-U55 NPU
bursting the convolutions on demand.

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
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/audio/audio-wake-word
west flash
```

On `native_sim` the PDM mic + Ethos-U paths NOSUPPORT-stub; the
loop still exercises the inference dispatch + post-process
plumbing and prints `[wake] done`. TODO(v0.6): drop the
Vela-compiled `hey_alp_vela.tflite` into `models/` and replace
the `s_model[]` placeholder.
