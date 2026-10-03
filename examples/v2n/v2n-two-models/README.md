# v2n-two-models

Run two models at the same time on an E1M-V2M, one on the RZ/V2N's on-die **DRP-AI3** NPU and one on the **DEEPX DX-M1**, each in its own thread, through `<alp/inference.h>`. Prints per-NPU latency and combined FPS.

> **`[UNTESTED on silicon]`.** Nothing here has run on an E1M-V2M. It builds against the public header only, and the two backends have never been run together. Each NPU has been run alone (DRP-AI3 YOLOX-S, DX-M1 yolo11n; see `docs/bring-up-drpai-v2n.md` and `docs/soms/v2n-m1.md`).

## What this shows

1. **Backend per handle.** `alp_inference_config_t.backend` picks the accelerator for that handle: `ALP_INFERENCE_BACKEND_DRPAI` for one, `ALP_INFERENCE_BACKEND_DEEPX_DXM1` for the other. Do not use `ALP_INFERENCE_BACKEND_AUTO`: it is fixed at build time and resolves to the DX-M1 on a build with both, so the DRP-AI handle must be named explicitly.
2. **One thread per NPU.** Each NPU runs one job at a time; the two NPUs are independent hardware, so a thread on each overlaps their work. Two threads on the same NPU would only take turns (the SDK serialises DRP-AI jobs with a process-wide lock, and the DX-M1 time-shares between engines).
3. **Solo, then both.** The default `all` mode runs DRP-AI alone, DX-M1 alone, then both, so the printed numbers show what sharing the A55 cores and DDR costs each NPU.
4. **CPU budget.** The program sets `TVM_NUM_THREADS=2` and `TVM_BIND_THREADS=0` (unless you exported them) so the DRP-AI TVM runtime's worker pool, which pins one thread per core by upstream TVM's default, does not fight the DX-M1 runtime's threads, `dxrtd` and the pre/post-processing for the 4 A55 cores. The values are a starting point; the effect is unmeasured.
5. **Time-shared NPUs and failures.** A failed DRP-AI job is not detectable (the runtime's `Run()` returns void), so a fast result is not proof of a correct one.

## Run

No compiled model ships in this repository (alp-sdk#2236). Provide both:

| Argument | What |
|---|---|
| `<drpai-model.tar>` | a `drpai_dir` bundle: `tar -cf model.tar -C <obj_dir> .` of the DRP-AI TVM compiler's output directory (see `examples/v2n/v2n-drpai-inference/README.md`) |
| `<drpai-frame.bin>` | the raw bytes of that model's input tensor |
| `<model.dxnn>` | a DEEPX model compiled with `dxcom` |
| `<dxnn-frame.bin>` | the raw bytes of that model's input tensor |
| `[seconds]` | length of each phase, default 10 |
| `[mode]` | `all` (default), `solo-drpai`, `solo-dx` or `both` |

```
v2n-two-models drpai.tar drpai_frame.bin yolo11n.dxnn dx_frame.bin 30
```

Expected output (shape only; numbers depend on the models):

```
[two-npu] v2n-two-models: DRP-AI + DX-M1, 30 s
[two-npu] drpai: handle open (1 input, 1 output)
[two-npu] deepx: handle open (1 input, 1 output)
[two-npu] temp start: thermal_zone0 = <mC>
[two-npu] phase: solo drpai
[two-npu] drpai: <n> invokes, avg <ms> ms, max <ms> ms, <fps> FPS
[two-npu] phase: solo deepx
[two-npu] deepx: <n> invokes, avg <ms> ms, max <ms> ms, <fps> FPS
[two-npu] phase: both
[two-npu] drpai: ...
[two-npu] deepx: ...
[two-npu] combined: <n> invokes in <s> s = <fps> FPS
[two-npu] temp end: thermal_zone0 = <mC>
[two-npu] done
```

## Build

Linux-only. Build it with the Yocto recipe `meta-alp-sdk/recipes-examples/alp-two-models-inference/` (add the package to your image), or cross-compile against the SDK sysroot as the header of `CMakeLists.txt` shows. Not built by twister: there is no Zephyr target (both NPUs are A55/Linux-owned).

## Image requirements

Both NPU stacks must be in the image **and** in `libalp_sdk`:

- DEEPX: `meta-deepx-m1` in `bblayers.conf` (the V2M machine configs enable it).
- DRP-AI3: `meta-rz-drpai` in `bblayers.conf`, **and** `RUHMI_DRPAI_TVM_DIR` set to an account-gated Renesas RUHMI checkout. Without RUHMI the SDK's DRP-AI backend is not compiled and `.backend = ALP_INFERENCE_BACKEND_DRPAI` fails with `ALP_ERR_NOSUPPORT`, even though the DRP-AI driver and runtime are in the image. See `docs/bring-up-drpai-v2n.md` section 8.

Applications on V2M should also know that `libdxrt` turns `SIGSEGV`/`SIGBUS`/`SIGABRT` into `exit(1)`; see `docs/soms/v2n-m1.md`.

## HIL

`tests/hil/v2m103-x-evk/v2m103-two-npu-*.yaml` (one process, two processes, 10-minute soak). Not bench-verified.
