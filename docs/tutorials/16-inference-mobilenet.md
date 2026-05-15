# Tutorial 16: Inference on Ethos-U (MobileNet)

**Target audience:** developers running ML inference on AEN
(Ethos-U55) or N93 (Ethos-U65) via `<alp/inference.h>`.

**Prerequisites:**

- Tutorial [01](01-first-build.md) completed.
- An AEN EVK or N93 EVK on the bench.
- A Vela-compiled MobileNet TFLite model (~600 KiB).  Build
  one with `pip install ethos-u-vela` + the upstream
  MobileNet v2 reference model.

**Outcome:** load a Vela-compiled model into the SDK's
inference dispatcher, run a single inference, read out the
classification result, compare against the CPU reference.

**Time:** 30-60 minutes (model prep is the long pole the first
time).

> **Note:** this tutorial walks the API + the build flow.  The
> end-to-end run depends on HiL availability for the Ethos-U
> path; the matching `docs/test-plan.md` row stays 🟡 until the
> nightly-aen-hil runner exercises it.  The code path described
> here is real + buildable today; what's gated is the silicon-
> level verification.

---

## 1. The inference dispatcher

`<alp/inference.h>` is a backend dispatcher.  The customer's
code is the same regardless of which NPU runs the model:

```c
alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
    .backend       = ALP_INFERENCE_AUTO,   // or _ETHOS_U, _DRPAI, _DEEPX_DX, _CPU
    .model_bytes   = mobilenet_tflite,
    .model_size    = sizeof(mobilenet_tflite),
    .arena_size    = 512 * 1024,
});
```

Backend selection:

| `backend` | Active on |
|-----------|-----------|
| `_AUTO` | Picks per SoM: Ethos-U on AEN/N93, DRP-AI on V2N, DEEPX on V2M, CPU fallback otherwise. |
| `_ETHOS_U` | AEN (E3..E8 with Ethos-U55) + N93 (Ethos-U65). |
| `_DRPAI` | V2N + V2N-M1 (DRP-AI3). |
| `_DEEPX_DX` | V2N-M1 (DEEPX DX-M1 on a PCIe-like link). |
| `_CPU` | Reference kernels; always available, slow. |

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

## 3. The application

```c
#include <stdio.h>
#include "alp/inference.h"
#include "mobilenet.h"   // generated above

int main(void) {
    alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
        .backend     = ALP_INFERENCE_AUTO,
        .model_bytes = vela_out_mobilenet_v2_1_0_224_quant_vela_tflite,
        .model_size  = vela_out_mobilenet_v2_1_0_224_quant_vela_tflite_len,
        .arena_size  = 512 * 1024,
    });
    if (inf == NULL) {
        printf("[inf] open failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    /* Set input -- MobileNet wants 224x224x3 uint8. */
    uint8_t *input  = alp_inference_input_tensor(inf, 0);
    /* Fill `input` with your image bytes (camera capture, test image, ...) */
    fill_test_image(input, 224 * 224 * 3);

    if (alp_inference_run(inf) != ALP_OK) {
        printf("[inf] run failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    /* Read output -- MobileNet emits 1001 class scores (uint8 quant). */
    const uint8_t *output = alp_inference_output_tensor(inf, 0);
    int top_idx   = 0;
    uint8_t top_v = 0;
    for (int i = 0; i < 1001; ++i) {
        if (output[i] > top_v) { top_v = output[i]; top_idx = i; }
    }
    printf("[inf] top-1 class %d score %u\n", top_idx, top_v);

    alp_inference_close(inf);
    return 0;
}
```

## 4. `board.yaml`

```yaml
schema_version: 2

som:
  sku: E1M-AEN701

carrier:
  name: E1M-EVK

cores:
  m55_hp:
    os: zephyr
    app: ./src
    inference:
      backend:    ethos_u       # explicit; or omit for auto
      arena_size: 524288        # 512 KiB
  m55_he:
    os: "off"                   # second M55 stays dark on this app
```

The `inference.backend` + `arena_size` translate to the
matching `CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y` +
`CONFIG_ALP_INFERENCE_ARENA_SIZE=524288` in the generated
config.

## 5. Build + flash

```bash
west alp-build -b alif_e7_dk_rtss_he examples/inference-mobilenet
west flash
```

Expected output (on real silicon):

```
[inf] open: model 612 KiB, backend ETHOS_U
[inf] tensor arena 512 KiB allocated
[inf] run: 7.2 ms (Ethos-U inference)
[inf] top-1 class 285 score 240  -- "Egyptian cat"
```

Under native_sim, the Ethos-U backend isn't available; `_AUTO`
falls back to CPU:

```
[inf] open: model 612 KiB, backend CPU (Ethos-U unavailable)
[inf] tensor arena 512 KiB allocated
[inf] run: 1240 ms (CPU inference)
[inf] top-1 class 285 score 240  -- "Egyptian cat"
```

The CPU and Ethos-U paths produce identical top-N classification
(within floating-point quantisation tolerance) -- the SDK's
test plan asserts this as the verification criterion in
[`docs/test-plan.md`](../test-plan.md).

## 6. Targeting V2N (DRP-AI) instead

Same `main.c`, swap `board.yaml`:

```yaml
som:
  sku: E1M-V2N101

carrier:
  name: E1M-X-EVK

os: yocto

inference:
  backend: drpai
```

But the model must be re-compiled with Renesas's **DRP-AI
translator** rather than Vela.  The translator produces a
`.drpai` artefact + a companion `.tflite`; the SDK's DRP-AI
backend reads both.  Customers writing portable apps that
target both Ethos-U and DRP-AI ship both model variants and
let `alp_inference_open` pick per-SoM at runtime.

## 7. Performance + memory budget

The arena size is the biggest config knob.  Vela's report
output (`vela-out/network_summary_*.csv`) tells you the
exact arena size the compiled model needs.  Round up to the
nearest 64 KiB:

```yaml
inference:
  arena_size: 524288     # 512 KiB; matches MobileNet v2 quant
```

Per-backend latency baselines (native_sim CPU + AEN Ethos-U55):

| Model | CPU (native_sim) | Ethos-U55 (AEN) |
|-------|------------------|------------------|
| MobileNet v2 224x224 quant | ~1240 ms | ~7 ms |
| MobileNet v2 96x96 quant | ~120 ms | ~1 ms |

Real numbers per silicon land in
`tests/bench/baselines/E1M-AEN701-zephyr.yaml` once HiL is
provisioned.

## 8. Troubleshooting

- **`alp_inference_open` returns NULL with NOSUPPORT** -- the
  selected backend isn't compiled in.  Check `board.yaml` and
  the generated `alp.conf`.
- **Open succeeds but `_run` returns ALP_ERR_NOMEM** -- arena
  too small for the model.  Bump `arena_size`; check Vela's
  output for the required value.
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
- [`docs/test-plan.md`](../test-plan.md) -- the verification
  ledger row for "Ethos-U inference matches CPU reference".
