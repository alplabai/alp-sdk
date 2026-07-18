<!-- Last verified: 2026-06-17 against AEN801 (E8) bench state. -->

# Tutorial 16: Inference on Ethos-U (MobileNet)

**Target audience:** developers running ML inference on AEN
(Ethos-U55 + Ethos-U85 on E4/E6/E8) or N93 (Ethos-U65) via
`<alp/inference.h>`.

**Prerequisites:**

- Tutorial [01](01-first-build.md) completed.
- An AEN EVK or N93 EVK on the bench.
- A Vela-compiled MobileNet TFLite model (~600 KiB).  Build
  one with `pip install ethos-u-vela` + the upstream
  MobileNet v2 reference model.  **You compile a different
  artefact per NPU variant** — see §6 below.

**Outcome:** load a Vela-compiled model into the SDK's
inference dispatcher, run a single inference, read out the
classification result, compare against the CPU reference.

**Time:** 30-60 minutes (model prep is the long pole the first
time).

> **Note:** this tutorial walks the API + the build flow.  The
> Ethos-U inference path is bench-verified on real AEN801 (E8)
> silicon: a TFLM + Ethos-U85 fixture runs to completion (RESULT
> PASS, flashed via J-Link direct MRAM).  The full 224x224
> MobileNet artefact in this tutorial is not yet HiL-exercised;
> the matching `docs/test-plan.md` row stays 🟡 until the
> nightly-aen-hil runner runs that model end-to-end.

---

## 1. The inference dispatcher

`<alp/inference.h>` is a backend dispatcher.  The customer's
code is the same regardless of which NPU runs the model:

```c
alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
    .backend     = ALP_INFERENCE_BACKEND_AUTO,
    .model_data  = mobilenet_vela_tflite,
    .model_size  = sizeof(mobilenet_vela_tflite),
    .format      = ALP_INFERENCE_MODEL_VELA,
    .arena_bytes = 512 * 1024,
});
```

Backend selection:

| `backend` | Active on |
|-----------|-----------|
| `_AUTO` | Picks per SoM: Ethos-U on AEN/N93, DRP-AI on V2N, DEEPX on V2M, CPU fallback otherwise. |
| `_ETHOS_U` | AEN (E3..E8 with Ethos-U55 + E4/E6/E8 with Ethos-U85) + N93 (Ethos-U65). |
| `_DRPAI` | V2N + V2M (DRP-AI3). |
| `_DEEPX_DXM1` | V2M101 / V2M102 (DEEPX DX-M1 on a PCIe-like link). |
| `_CPU` | TFLM reference / Helium / NEON kernels; always available. |

The `ETHOS_U` token is a single customer-facing handle that
covers every Arm Ethos NPU variant.  The orchestrator emits
per-variant `CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U{55,65,85}=y`
gates from the SoM preset's `inference.npu_population[]` (G-1
selector); the driver code at runtime dispatches to the right
shim and logs the active variant once per boot
(`alp_inference_tflm_npu_variant_name()`).  Customers don't
touch these — Vela picks at model-compile time and the SDK
takes care of the rest.

A `_AUTO` open on AEN routes to Ethos-U if the model is Vela-
compiled (Ethos-U requires the Vela pre-compilation step);
falls back to CPU otherwise.

## 2. Prepare a Vela-compiled MobileNet

Pre-compile the model offline on your host:

```bash
pip install ethos-u-vela

vela mobilenet_v2_1.0_224_quant.tflite \
    --accelerator-config ethos-u55-256 \
    --output-dir vela-out/

# Output: vela-out/mobilenet_v2_1.0_224_quant_vela.tflite
```

Convert to a C header for embedding in the firmware:

```bash
xxd -i vela-out/mobilenet_v2_1.0_224_quant_vela.tflite \
    > app/include/mobilenet.h

# Output: a C array + size constant; include in main.c.
```

> **Variant matters at the artefact level.**  `vela
> --accelerator-config ethos-u55-256` targets the U55; for U85
> on AEN E4/E6/E8, recompile with `ethos-u85-256` (or another
> U85 config string).  The compiled `.tflite` is **NOT
> portable across Ethos-U variants** — Vela picks the
> operator-fusion + kernel set at compile time.  See §6
> for the cross-NPU story.

## 3. The application

```c
#include <stdio.h>
#include "alp/inference.h"
#include "mobilenet.h"   // generated above

int main(void) {
    alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .model_data  = vela_out_mobilenet_v2_1_0_224_quant_vela_tflite,
        .model_size  = vela_out_mobilenet_v2_1_0_224_quant_vela_tflite_len,
        .format      = ALP_INFERENCE_MODEL_VELA,
        .arena_bytes = 512 * 1024,
    });
    if (inf == NULL) {
        printf("[inf] open failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    /* Set input -- MobileNet wants 224x224x3 uint8. */
    alp_inference_tensor_t in;
    alp_inference_get_input(inf, 0, &in);
    fill_test_image((uint8_t *)in.data, 224 * 224 * 3);

    if (alp_inference_invoke(inf) != ALP_OK) {
        printf("[inf] invoke failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    /* Read output -- MobileNet emits 1001 class scores (uint8 quant). */
    alp_inference_tensor_t out;
    alp_inference_get_output(inf, 0, &out);
    const uint8_t *scores = (const uint8_t *)out.data;
    int top_idx   = 0;
    uint8_t top_v = 0;
    for (int i = 0; i < 1001; ++i) {
        if (scores[i] > top_v) { top_v = scores[i]; top_idx = i; }
    }
    printf("[inf] top-1 class %d score %u\n", top_idx, top_v);

    alp_inference_close(inf);
    return 0;
}
```

## 4. `board.yaml`

```yaml
som:
  sku: E1M-AEN801

preset: e1m-evk
cores:
  m55_hp:
    app: ./src                    # os: omitted -- M-cores default to zephyr
    inference:
      default_arena_kib: 512      # 512 KiB scratch for MobileNet v2 quant
  m55_he:
    os: "off"                     # second M55 stays dark on this app
```

There is no `inference.backend:` field — the dispatcher set is
silicon-determined. AEN801's SoM preset declares the U85 primary plus
the U55 pair via `inference.npu_population[]` (with fallback
capability counters), so the loader emits:

```
CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y           # G-2: M55 Helium MVE kernels
CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y
CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y           # G-1: per-variant U55 gate
```

…automatically, alongside the universal TFLM CPU fallback.
For AEN801 (which carries the U85 too) the loader would also
emit `CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y`; for N93 it
emits `_U65=y` plus `CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y`
(the i.MX 93 PHY shim).  Advanced readers: the emit logic
lives in `scripts/alp_orchestrate/` § *Per-variant Ethos-U
selector* (G-1) and § *CPU-class TFLM kernel selector* (G-2).

The app picks Ethos-U vs CPU per-handle at runtime via
`alp_inference_open(.backend = ALP_INFERENCE_BACKEND_AUTO)` (or
an explicit value).

## 5. Build + flash

```bash
tan build -b alif_e7_dk_rtss_he examples/aen/edgeai-vision-aen
west flash
```

Expected output (on real silicon):

```
[inf] open: model 612 KiB, backend ETHOS_U (variant U55)
[inf] tensor arena 512 KiB allocated
[inf] invoke: 7.2 ms (Ethos-U inference)
[inf] top-1 class 285 score 240  -- "Egyptian cat"
```

Under native_sim, the Ethos-U backend isn't available; `_AUTO`
falls back to CPU:

```
[inf] open: model 612 KiB, backend CPU (Ethos-U unavailable)
[inf] tensor arena 512 KiB allocated
[inf] invoke: 1240 ms (CPU inference, kernels=REF)
[inf] top-1 class 285 score 240  -- "Egyptian cat"
```

The CPU and Ethos-U paths produce identical top-N classification
(within floating-point quantisation tolerance) -- the SDK's
test plan asserts this as the verification criterion in
[`docs/test-plan.md`](../test-plan.md).

## 6. Targeting V2N (DRP-AI) instead -- the cross-NPU story

Same `main.c` API surface, swap `board.yaml`:

```yaml
som:
  sku: E1M-V2N101
preset: e1m-x-evk
cores:
  m33_sm:
    app: ./src
```

No backend pick needed in `board.yaml`.  Note the core-class
split on V2N: the DRP-AI3 engine is driven from the **A55/Linux
side only** (the MERA runtime — `capabilities.drp_ai: true` makes
the loader emit `-DALP_SDK_USE_DRPAI_V2N=ON` on the A55/Yocto
slice), so the `m33_sm` Zephyr slice above runs the model on the
**TFLM CPU backend** — there is deliberately no M-class DRP-AI
backend (issue #58).  The app still calls `alp_inference_open`
with the same `.backend = ALP_INFERENCE_BACKEND_AUTO`, and the
dispatcher routes to Ethos-U on AEN silicon (the M55 drives that
NPU directly), TFLM on the V2N M33, CPU under native_sim — same
source, three SoMs.

> **The one thing the SDK cannot abstract: the model
> artefact.**  Each NPU vendor ships its own offline compiler
> that lowers a `.tflite` (or other source format) to that
> NPU's instruction set.  The compiled artefacts are NOT
> interchangeable:
>
> | NPU                  | Compiler                  | Produces                    |
> |----------------------|---------------------------|-----------------------------|
> | Ethos-U55 (AEN)      | `vela --accelerator-config ethos-u55-256`   | `.tflite` (Vela-rewritten)  |
> | Ethos-U85 (AEN E4/E6/E8) | `vela --accelerator-config ethos-u85-256` | `.tflite` (Vela-rewritten)  |
> | Ethos-U65 (N93)      | `vela --accelerator-config ethos-u65-256`   | `.tflite` (Vela-rewritten)  |
> | DRP-AI3 (V2N / V2M)  | Renesas DRP-AI TVM        | DRP-AI runtime dir          |
> | DEEPX DX-M1 (V2M)    | `dxcom` (license-gated)   | `.dxnn` binary              |
>
> For **Ethos-U** the SDK runs Vela automatically as part of `alp model build`
> (no extra `board.yaml` needed).  For **DRP-AI** and **DEEPX** the toolchains
> require per-model configuration (a DRP-AI TVM spec file, or a DEEPX JSON
> config + calibration dataset).  Supply these in `board.yaml` under
> `models[].compile:` so `alp model build` can invoke them:
>
> ```yaml
> models:
>   - name: mobilenet
>     source: models/mobilenet.onnx
>     compile:
>       drpai:
>         spec: models/mobilenet.drpai.yaml   # DRP-AI TVM compile spec
>       deepx_dxm1:
>         config:      models/mobilenet.deepx.json   # dxcom per-model JSON
>         calibration: models/calib/                 # PTQ calibration dataset
> ```
>
> A backend with no `compile:` block is recorded as
> `coverage: skipped ("no compile config")` in the `.alpmodel` manifest —
> the build still succeeds and produces blobs for every backend whose config
> is present.  Real DRP-AI / DEEPX compilation (Stage 2) is gated on the
> respective toolchain being installed; see the local-CI guide.
>
> Customers writing one-source-multiple-target apps ship every
> model variant they want to run; the on-device `alp_inference_open_alpmodel()`
> loader picks the right blob automatically from the `.alpmodel` package.
>
> This is **what you can't abstract** — the C surface stays
> uniform, but the bytes a model compiles to are
> NPU-instruction-set-specific.  Plan model artefact storage +
> OTA flows accordingly.

## 6b. Concurrent multi-NPU on V2M101 (V2N + DEEPX)

V2M101 ships both DRP-AI3 (on the V2N host silicon) and DEEPX
DX-M1 (on-module via PCIe).  The SoM preset declares both:

> **Note**: the `capabilities:` block in a SoM preset is an SDK-internal field — the loader reads it (merged with the SoC JSON's `capabilities:` per silicon defaults) and surfaces the merged dict to backends. You never write `capabilities:` in your own `board.yaml`. Shown here for illustration only.

```yaml
# metadata/e1m_modules/E1M-V2M101.yaml
capabilities:
  drp_ai:      true
  deepx_dxm1:  true
```

Both engines live on the **A55/Linux side** (DRP-AI3 via the MERA
runtime, DX-M1 via `libdxrt` over the A55's PCIe): the loader
emits `-DALP_SDK_USE_DRPAI_V2N=ON` and `-DALP_SDK_USE_DEEPX_DXM1=ON`
for the A55/Yocto slice and compiles both dispatchers in.  Apps can
open **multiple handles** in parallel, each bound to a different
NPU, to run independent models concurrently:

```c
alp_inference_t *m_vision = alp_inference_open(&(alp_inference_config_t){
    .model_data  = vision_dxnn,
    .model_size  = sizeof(vision_dxnn),
    .format      = ALP_INFERENCE_MODEL_DXNN,
    .backend     = ALP_INFERENCE_BACKEND_DEEPX_DXM1,   // dispatch to DEEPX
    .arena_bytes = 256 * 1024,
});

alp_inference_t *m_audio = alp_inference_open(&(alp_inference_config_t){
    .model_data  = audio_drpai,
    .model_size  = sizeof(audio_drpai),
    .format      = ALP_INFERENCE_MODEL_DRPAI,
    .backend     = ALP_INFERENCE_BACKEND_DRPAI,      // dispatch to DRP-AI3
    .arena_bytes = 128 * 1024,
});

/* Run independent models on the two NPUs concurrently. */
alp_inference_invoke(m_vision);   /* DEEPX over PCIe */
alp_inference_invoke(m_audio);    /* DRP-AI3 on-die */
```

The runtime per-handle `backend` field is what makes the
concurrent-multi-NPU pattern possible.  `board.yaml` doesn't need
to (and can't) declare which backend "wins" — both compile in,
and the app picks per inference call.

## 7. Performance + memory budget

The arena size is the biggest config knob.  Vela's report
output (`vela-out/network_summary_*.csv`) tells you the
exact arena size the compiled model needs.  Round up to the
nearest 64 KiB:

```yaml
cores:
  m55_hp:
    app: ./src
    inference:
      default_arena_kib: 512   # 512 KiB; matches MobileNet v2 quant
```

Per-backend latency baselines (native_sim CPU + AEN Ethos-U55):

| Model | CPU (native_sim) | Ethos-U55 (AEN) |
|-------|------------------|------------------|
| MobileNet v2 224x224 quant | ~1240 ms | ~7 ms |
| MobileNet v2 96x96 quant | ~120 ms | ~1 ms |

Real numbers per silicon land in
`tests/bench/baselines/E1M-AEN801-zephyr.yaml` once HiL is
provisioned.

## 8. Troubleshooting

- **`alp_inference_open` returns NULL with NOSUPPORT** -- the
  selected backend isn't compiled in.  Check `board.yaml` and
  the generated `alp.conf`.
- **Open succeeds but `_invoke` returns ALP_ERR_NOMEM** --
  arena too small for the model.  Bump `arena_bytes`; check
  Vela's output for the required value.
- **Open fails with format mismatch** -- you passed an Ethos-U
  Vela-compiled `.tflite` but ran on V2N (which needs DRP-AI
  artefacts) or vice versa.  Match `format:` to the SoM you're
  targeting; see §6 for the per-NPU artefact table.
- **Classification result differs CPU vs NPU** -- expected at
  the quantisation level (Vela may shuffle the op order in
  ways that change low-order bits).  Top-N classes should
  match; raw scores can differ ±2 LSBs.
- **Random top-1 result** -- input tensor not initialised
  correctly.  Confirm the byte order + the colourspace
  (MobileNet expects RGB; some camera paths produce BGR).

## See also

- [`<alp/inference.h>`](../../include/alp/inference.h) -- the
  public API.
- [`examples/aen/edgeai-vision-aen/`](../../examples/aen/edgeai-vision-aen/)
  -- end-to-end demo: camera → ISP → inference → OLED overlay.
- [Vela docs](https://github.com/ARM-software/ethos-u-vela) --
  the Ethos-U pre-compiler.
- [`scripts/alp_orchestrate/`](../../scripts/alp_orchestrate/)
  § *Per-variant Ethos-U selector* + § *CPU-class TFLM kernel
  selector* -- the G-1 / G-2 emit logic for advanced readers.
- [`docs/test-plan.md`](../test-plan.md) -- the verification
  ledger row for "Ethos-U inference matches CPU reference".
