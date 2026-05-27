# Stage 2 — Real NPU compilers + runtimes (DRP-AI + DEEPX) — design

- **Date:** 2026-05-27
- **Status:** Design / grounding (kickoff for the Stage-2 cycle — no code yet beyond the adapter probes)
- **Scope:** Turn the two NOT_IMPLEMENTED NPU paths (`drpai`, `deepx_dxm1`) into real **host compilers** (the `scripts/alp_model/adapters/` side) and real **A55/Linux runtimes** (the `src/yocto/` side), against the actual vendor toolchains. The `.alpmodel` contract (Stages 1a–1c) is **frozen** and already reserves both backends — Stage 2 slots in with **no format change**.
- **Builds on:** [[project_unified_model_pipeline]] (Stage 2), [[reference_deepx_toolchain]], the 1c selection engine/loader, the backend registry, and `src/yocto/inference_yocto.c` (the Linux dispatch that already routes `ALP_INFERENCE_BACKEND_DEEPX_DXM1`).

---

## 1. Where these backends live (both are Yocto/A55, not Zephyr/M-class)

On the V2N / V2N-M1 SoMs the NPUs are reached from **Linux on the A55 cluster**, not from the M33:
- **DRP-AI** is in the RZ/V2N SoC itself; the A55 runs the DRP-AI TVM runtime against it.
- **DEEPX DX-M1** is a companion accelerator behind **PCIe Gen3** (see `metadata/socs/deepx/dx/m1.json`); the A55 runs `dx_rt` over the PCIe NPU driver.

So both runtime backends are **`src/yocto/` files** dispatched by `inference_yocto.c`'s `switch(cfg.backend)` (gated by `ALP_SDK_USE_DRPAI` / `ALP_SDK_USE_DEEPX_DXM1`). The Zephyr/M-class registry path is **not** involved for these two. (The 1c loader already folds `deepx:dx:m1` into `avail_silicon` when `ALP_SDK_USE_DEEPX_DXM1` / `CONFIG_ALP_SDK_INFERENCE_BACKEND_DEEPX_DXM1` is set, and on Yocto the loader's `cfg.backend` drives the dispatch directly.)

## 2. The two toolchains (grounded 2026-05-27)

### DRP-AI — open source
- **Compiler:** `github.com/renesas-rz/rzv_drp-ai_tvm` — a **TVM-based** compiler (the DRP-AI Translator extended to a TVM backend). **Build from source / Docker** (NOT license-gated). Input **ONNX**; output is **"Runtime Model Data"** (a *directory* of files) deployed to the board alongside the DRP-AI TVM runtime library + a C++ inference app. Compile flow = Python scripts (see the repo's `tutorials/`). Targets RZ/V2L/V2M/V2MA/V2H/**V2N**.
- **Runtime:** the DRP-AI TVM **C++ runtime library** on the A55; "CPU and DRP-AI work together" for parts of a model. Obtained by building the open repo (`setup/`).

### DEEPX — proprietary, license-gated
- **Compiler:** `dxcom` — the console script of the proprietary **`dx-com` Python wheel** (verified: `dx_com-2.3.0`, `cp312`, `manylinux_2_31_x86_64`, `Requires-Python <3.13,>=3.8`, `License :: Other/Proprietary`, ONNX frontend via bundled `onnx_frontend`/`quant_fx`). CLI: `dxcom -m <model.onnx> -c <config.json> -o <out_dir>` with a per-model **JSON config + calibration dataset** (post-training quant). The wheel is a **license-gated download** (developer portal) and **not redistributable** — alp-sdk references the public `github.com/DEEPX-AI` repos (`dx-compiler`, `dx_rt`, `dx_app`, `dx_rt_npu_linux_driver`) and describes the wheel by role; it never bundles or links the portal.
- **Runtime:** `dx_rt` (C++ inference framework; Python/C# bindings) on the A55 over **`dx_rt_npu_linux_driver`** (PCIe), `debian_m1` target. The C++ API is "limited rights" (not public) — grounding it needs the licensed SDK.

**Both ingest ONNX** → so ONNX is the canonical NPU source format; a `.tflite` model targeting DRP-AI/DEEPX is a `tflite→onnx` documented pre-step. (This is why the 1c follow-up notes ONNX-compile + why `DrpaiAdapter`/`DeepxAdapter.accepts()` are now ONNX-only.)

## 3. Host-compile side — `scripts/alp_model/adapters/`

The adapters are already detect-and-skip and now probe correctly (`dxcom` on PATH / `ALP_DEEPX_SDK_HOME`; `ALP_DRPAI_TVM_HOME`) and accept ONNX. Stage 2 makes `compile()` real — mirroring the `VelaAdapter` shell-out + skip-gated pattern:

- **DeepxAdapter.compile():** `dxcom -m <source.onnx> -c <config.json> -o <out>` → read back the output, return a `Blob`. The DEEPX wheel is installable in WSL (Linux/py3.12) for host testing.
- **DrpaiAdapter.compile():** run the DRP-AI TVM compile flow (the repo's Python compiler entry) → read back the Runtime Model Data dir.

**New plumbing required — per-model compile config + calibration.** Unlike Vela (`--accelerator-config` is derived), both vendors need a *per-model* config + calibration set that the SDK can't derive. Proposed `board.yaml` `models:` extension (Stage-2 design decision — confirm shape; shown as text since it intentionally extends the frozen `models[]` schema):

```text
models:
  - name: person_detect
    source: models/person_detect.onnx
    compile:
      deepx:  { config: models/person_detect.deepx.json, calibration: models/calib/ }
      drpai:  { spec:   models/person_detect.drpai.yaml }
```

`build_model` passes the matching `compile.<backend>` block to the adapter. Backends without a block fall to a `coverage: skipped` ("no compile config") rather than guessing.

**Blob shape (TBD against a real compile):** both compilers emit **directories**, so the `.alpmodel` blob is likely a **tar of the output dir** (as the spec already anticipates for `drpai_dir`). DEEPX's exact output (single `.dxnn` vs a dir) must be confirmed from a real `dxcom` run; the manifest `blob_format` is `drpai_dir` / `dxnn` accordingly (extend with `deepx_dir` if needed).

## 4. Runtime side — `src/yocto/`

- **`src/yocto/inference_deepx.cpp`** (exists as wiring): implement `alp_inference_deepx_{open,invoke,get_input,get_output,close}` against the **`dx_rt`** C++ API (load the DEEPX blob, run over PCIe). Needs `dx_rt` headers/libs on the Yocto sysroot + `dx_rt_npu_linux_driver` in the image.
- **`src/yocto/inference_drpai.cpp`** (new): implement the DRP-AI hooks against the **DRP-AI TVM runtime library**.
- Both link into `inference_yocto.c`'s dispatch under `ALP_SDK_USE_*`. The `meta-alp` BSP layer needs recipes for the PCIe driver + the runtime libs.

## 5. What's gated vs doable-now

| Piece | Gating |
|---|---|
| DeepxAdapter / DrpaiAdapter **probes + ONNX accept + docs** | **DONE** (this branch) — grounded, no tools needed. |
| `models:` compile-config/calibration **plumbing** | Doable now (schema + build_model + adapter signature) — pure host Python. |
| Real `DeepxAdapter.compile()` (dxcom) | **DONE (2026-05-27)** — implemented + validated against the real `dx-com` 2.3.0 wheel in a WSL py3.12 venv. Confirmed CLI `dxcom -m <onnx> -c <config.json> -o <OUTPUT_DIR>` (a directory → `blob_format: deepx_dir`, tar the dir); calibration is referenced from the JSON config (no CLI flag); version banner "DX-COM (DEEPX Compiler) 2.3.0". Covered by a mocked shell-out test + a `which("dxcom")` version smoke (passes against the real wheel). A full end-to-end real-compile test still needs a DEEPX sample (ONNX + config + calibration). |
| Real `DrpaiAdapter.compile()` (TVM) | Needs the **open DRP-AI TVM toolchain built** (source/Docker) + a sample ONNX. |
| `dx_rt` / DRP-AI TVM **runtime backends** | Needs the **licensed dx_rt SDK** (DEEPX) + runtime libs + **bench DX-M1 / RZ-V2N silicon**. |

## 6. Build order (Stage-2 cycle)

1. **Compile-config plumbing** (`models: compile:` schema + `build_model` + adapter `compile(..., opts)` signature) — host Python, no tools, fully testable.
2. **Real `DeepxAdapter.compile()`** — **DONE (2026-05-27).** Installed the `dx-com` 2.3.0 wheel in a WSL py3.12 venv; confirmed `dxcom --help` (`-m`/`-c`/`-o`, `-o` is a dir); implemented the shell-out (tar the output dir, `blob_format: deepx_dir`); mocked test + `which("dxcom")` version smoke (mirrors the VelaAdapter pattern). Remaining: a full end-to-end real-compile test needs a DEEPX sample (ONNX + config + calibration).
3. **Real `DrpaiAdapter.compile()`** — build the open DRP-AI TVM toolchain, same pattern.
4. **Yocto runtime backends** (`inference_deepx.cpp` real via dx_rt; new `inference_drpai.cpp`) + the `meta-alp` PCIe-driver / runtime-lib recipes — **bench-gated**.

## 7. Open questions
- ~~Exact `dxcom` output structure (dir vs single `.dxnn`)~~ → **RESOLVED**: `dxcom -o` is a **directory** → `blob_format: deepx_dir`, tar the dir. The exact filenames inside (e.g. a `.dxnn` + metadata) await a real compile but don't change the tar-the-dir decision. The DRP-AI Runtime-Model-Data layout is still TBD (gated on the TVM build).
- The DRP-AI TVM compile **entry command** (Python script under the repo) → confirm from `rzv_drp-ai_tvm/tutorials/`.
- The `models: compile:` schema shape (per-backend block vs generic `compile_opts`) → decide in step 1.
- `dx_rt` C++ API surface → needs the licensed DEEPX SDK docs.
