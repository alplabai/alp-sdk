# aen-inference-latency — cycles and milliseconds per inference, no power monitor

Runs a Vela-compiled int8 model on the Ethos-U85 of the E1M-AEN801 / AEN803
(Ensemble E8, M55-HE) in timed windows and prints how long one inference takes.
It is the latency-only sibling of [`aen-inference-energy`](../aen-inference-energy/):
no INA236, no I2C, no chip driver — only the M55 DWT cycle counter and the console.
Board target `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he` (the AEN801 twin is
identical silicon + PCB; both overlays ship in `boards/`).

## What it measures

Each window runs back-to-back `Invoke()` calls (default 20 inferences, at least 50 ms,
at most 10 s) and reports the window's whole span in CPU cycles and milliseconds plus
the inference count. Latency is span / inferences: warm, model already allocated,
Ethos-U command build + NPU run + interrupt + output hand-off + any CPU fallback ops.
It is not pure NPU compute time and excludes the one-off `AllocateTensors` / warm-up.
Nothing prints inside a window.

Peak SRAM is the tensor-arena high-water mark (`arena_used_bytes`, from the TFLM
planner); `sram_peak_bytes` is that plus the model copy staged in SRAM0.

## Console protocol

A strict subset of `aen-inference-energy`'s `ENERGY-*` protocol, so
`tan model run --device --capture FILE` parses both apps with the same code:

```
ENERGY-CFG {"mode":"latency-only","cycles_per_s":160000000,"npu_dispatched":true,"model":"<name>","model_bytes":N,"arena_used_bytes":N,"arena_bytes":N,"sram_peak_bytes":N,"windows":W,"inferences_per_window":N,"timestamp_source":"dwt-cyccnt"}
ENERGY-W <window> active 0 <span_cycles> <span_ms> <inferences>
ENERGY-WERR <window> active timed_out=1 invoke_status=<rc> completed=<n>   (failed window only)
ENERGY-WARN active window <ms> ms exceeds the cycle-counter wrap ...       (only if a window wrapped)
```

The parser requires `cycles_per_s` (> 0) and `npu_dispatched` in the header and at
least one `ENERGY-W` line with a positive span and at least one inference. There is
no idle phase, no `ENERGY-S` stream and no `rail` / `power_lsb_w`, so the host derives
latency only and no energy. Lines the parser skips as noise, kept for humans:

```
LATENCY-I <window> <j> <cycles>                      first 32 inferences of each window
LATENCY-WSTAT <window> n=.. min=.. max=.. mean=..    cycles per inference
LATENCY-RESULT {"cycles_per_inference":..,"ms_per_inference":..,...}
RESULT PASS|FAIL: <cycles> cycles/inference (<ms> ms) ...
```

## Build + run — Flow C (small models, ITCM RAM-run)

The board overlay already retargets ROM to ITCM, so a plain build links at `0x0`.
The tiny-fixture image is 83324 B of the 256 KiB ITCM (code is about 80 KB, so a Vela
model up to roughly 170 KB fits; the model is rodata in ITCM and is staged to SRAM0 at
boot). The console is the RAM console (`ram_console_buf`, 8 KiB, DTCM), read over SWD.

```sh
A=$PWD/examples/aen/aen-inference-latency
scripts/bench/aen/build.sh "$A" \
  -DAEN_NPU_MODEL=<model_int8>.tflite -DAEN_NPU_MODEL_NAME=<name>
```

Then RAM-run it (`tan flash --ram`, or `scripts/bench/aen/ram-run.sh`) and save the
console to a file; `tan model run --device --capture FILE` turns it into the host-tier
envelope. Hold the labgrid reservation first, as for any bench work.

## Build + run — Flow D (models too big for ITCM)

As for `aen-inference-energy`: add
`-DEXTRA_DTC_OVERLAY_FILE="$A/flowd/mram-slot0.overlay"` and
`-DEXTRA_CONF_FILE="$A/flowd/mram-slot0.conf"` and write the image to MRAM slot0.
The RAM console is still the capture source.

## Knobs

`-DAEN_LATENCY_WINDOWS=<n>` (5), `-DAEN_LATENCY_INFERENCES=<n>` (20),
`-DAEN_LATENCY_WINDOW_MAX_MS=<ms>` (10000; keep it under the 26.8 s cycle-counter wrap
at 160 MHz).

## Limits

The model is Vela-compiled for `ethos-u85-256` by the build and must use ops in the
8-op resolver in `src/main.cpp`; anything else fails `AllocateTensors` and prints
`RESULT FAIL`. The model is baked in at build time: "any model" means any `.tflite`
passed to `-DAEN_NPU_MODEL`, not a runtime-loaded one.
