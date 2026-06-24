<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-npu-inference-person-mram — real-model Ethos-U85 inference from MRAM

Runs the **real `person_detect` MobileNet** (int8) end-to-end on the
**E1M-AEN801** (Alif Ensemble **E8**, Cortex-M55 RTSS-HE) Ethos-U85 (256-MAC),
with the model **resident in MRAM slot0**. It is the production-scale companion
to the hermetic `aen-npu-inference-alif` fixture: person_detect Vela'd for the
U85 is ~263 KiB and **overflows the 256 KiB ITCM RAM-run region**, so this
variant links into the MRAM **slot0** boot partition (`0x80010000`) and is
flashed + SE-booted via **Flow D** (J-Link direct MRAM).

The matched-runtime fix is identical to the fixture: two in-app **strong**
weak-overrides (`src/main.cpp`) — `ethosu_address_remap = local_to_global()` and
`ethosu_config_select()` returning the SRAM AXI port — drive the `ethosu_invoke`
path correctly. The model rodata lives in MRAM (XIP) and is `memcpy`'d into an
**SRAM0** buffer at boot; the NPU reads only SRAM0, `CONFIG_DCACHE=n`.

> **Bench-verified on E8 (2026-06-17):** Vela `ethos-u85-256` reports
> `CPU operators = 0 (0.0%) / NPU operators = 44 (100.0%)`, ~7.1M MACs; the SE
> boots the slot0 image and `runJob` completes with a populated output —
> `RESULT PASS: NPU inference complete -- model=person_detect_u85 runJob=OK`.

## What's different from the `-alif` fixture

| | `aen-npu-inference-alif` | `aen-npu-inference-person-mram` |
| --- | --- | --- |
| Model | `tiny_int8` fixture (8 MACs) | `person_detect` MobileNet (~7.1M MACs) |
| Link / boot | ITCM RAM-run (≤256 KiB) | **MRAM slot0** `0x80010000` |
| Flash | Flow D single-blob (ITCM ATOC) | **Flow D two-blob** (app + ATOC) |
| Key Kconfig | — | **`CONFIG_USE_DT_CODE_PARTITION=y`** (links at the slot0 offset; without it the image links at `0x80000000` and faults) |

## Build

Silicon-only (board `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`); needs the
alp-sdk, hal_alif, hal/ethos_u, and lib/tflite-micro modules plus Vela:

```sh
west build -p always \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-npu-inference-person-mram \
  -- \
  -DEXTRA_ZEPHYR_MODULES="<alp-sdk>;<hal_alif>;<hal/ethos_u>;<lib/tflite-micro>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-npu-inference-person-mram/boards/alp_e1m_aen801_m55_he.overlay \
  -DAEN_NPU_VELA=<venv>/bin/vela \
  -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/.../ensemble_vela.ini
```

`AEN_NPU_MODEL` defaults to `person_detect.tflite` from the tflite-micro module
(`${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/...`); `AEN_NPU_INPUT_BYTES` defaults to 9216
(96×96×1). `gen_model.py` re-Vela's the model at configure time → `model.h`. See
`aen-npu-inference-alif/README.md` for the full `-D` table and licensing
(`ensemble_vela.ini` is Alif-proprietary, **alp-sdk-internal** only).

### Mixed NPU/CPU models

The `ethosu_utils` op-resolver registers `AddEthosU()` **plus** the common CPU
reference kernels (`SVDF`/`FullyConnected`/`Softmax`/`Quantize`), so a model whose
Vela partition leaves ops on the M55 (not just the single fused Ethos-U op) also
runs — not only 100 %-NPU models. Set `AEN_NPU_INPUT_BYTES` to the model's input
size when swapping. Bench-verified on E8 (2026-06-17) with the mixed
keyword-spotting model (6 NPU / 9 CPU ops):

```bash
west build ... \
  -DAEN_NPU_MODEL=${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro/models/keyword_scrambled_8bit.tflite \
  -DAEN_NPU_MODEL_NAME=keyword_scrambled_u85 \
  -DAEN_NPU_INPUT_BYTES=192   # [1,96] int16
# -> RESULT PASS: runJob=OK out_bytes=8 (the SVDF/Softmax/Quantize run on the M55)
```

Sanity-check the link before flashing: `xxd -e -l 8 zephyr.bin` word 2 must read
`0x8001xxxx` (slot0). `0x8000xxxx` means `CONFIG_USE_DT_CODE_PARTITION` is missing.

## Run (Flow D — MRAM-XIP two-blob)

Two blobs are written over SWD: the app at `0x80010000` and the signed ATOC near
the top of slot0 (its address is parsed per-build, never hardcoded). The SES
boot ROM (re-run by the `RSetType 2` pin reset) verifies + boots it. The single
`RESULT` line is read from `ram_console_buf` over SWD — see the
`scripts/bench/aen/` helpers and `docs/aen-bench-bringup.md` §Flow D. Expected:

```
=== aen-npu-inference-person-mram (matched runtime, MRAM slot0) ===
model      : person_detect_u85 (262768 bytes)
npu ready  : 1 (Ethos-U85 @0x49042000)
RESULT PASS: NPU inference complete -- model=person_detect_u85 runJob=OK out_bytes=2
```
