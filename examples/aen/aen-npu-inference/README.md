<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-npu-inference — Ethos-U85 inference (E1M-AEN801)

The first on-silicon NPU inference example for the **E1M-AEN801** (Alif Ensemble
**E8**, Cortex-M55 RTSS-HE): a Vela-compiled int8 model flows through TFLM's
`MicroInterpreter`, whose Ethos-U custom-op resolver (`AddEthosU()`) dispatches
the fused subgraph onto the Ethos-U85 (256-MAC) via the Arm Ethos-U core driver
(`ethosu_invoke`). It is the inference follow-up to `aen-npu-ethosu-regcheck`
(which proved the U85 is present + clocked).

> **Looking for the bench-validated `RESULT PASS`?** This is the original
> end-to-end wiring; the **matched-runtime** fix that resolved the residual
> `ethosu_invoke=1` lives in the sibling `aen-npu-inference-alif` (a strong
> in-app `ethosu_address_remap`/`ethosu_config_select`). The Vela-config
> injection documented below is identical across all three NPU examples.

## Build

Silicon-only example. It needs the alp-sdk, hal_alif, hal/ethos_u, and
lib/tflite-micro modules plus the Vela toolchain at build time:

```sh
west build -p always \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-npu-inference \
  -- \
  -DEXTRA_ZEPHYR_MODULES="<alp-sdk>;<hal_alif>;<hal/ethos_u>;<lib/tflite-micro>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-npu-inference/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay \
  -DAEN_NPU_VELA=<venv>/bin/vela \
  -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/.../ensemble_vela.ini
```

`-D` cache overrides (see `CMakeLists.txt` for defaults):

| Var | Purpose |
| --- | --- |
| `AEN_NPU_VELA` | Vela executable (default: the `.venv-vela/bin/vela` scratch env). |
| `AEN_NPU_VELA_CONFIG` | **Alif-proprietary** `ensemble_vela.ini` — see *Vela system-config injection*. |
| `AEN_NPU_MODEL` | Input `.tflite` (default: the repo `tiny_int8.tflite` fixture). |
| `AEN_NPU_MODEL_NAME` | Embedded model identity string printed on the `RESULT` line. |
| `AEN_NPU_ACCEL` | Vela `--accelerator-config` (default `ethos-u85-256`). |

`gen_model.py` Vela-compiles `AEN_NPU_MODEL` at configure time and emits
`model.h` into the build tree (a `const` rodata array `memcpy`'d into an
uninitialized **SRAM0** buffer at boot — the Ethos-U DMA cannot reach the DTCM
that is the board's default `zephyr,sram`, and initialized `SRAM0` sections are
**not** init-copied). No model data is committed.

## Vela system-config injection (Alif-proprietary `ensemble_vela.ini`)

The Alif Vela **system-config** (`ensemble_vela.ini`) declares the
`Ethos_U85_SRAM_*` system-configs + memory modes that the bench-validated E8
RTSS-HE runtime was matched against. That file is **Alif-proprietary** and is
**not** vendored or redistributed by alp-sdk — it ships in the private
**alp-sdk-internal** repo. The example never carries the `.ini`; it is *injected
at build time* via a CMake cache var:

```sh
-DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/.../ensemble_vela.ini
```

- **Default (`AEN_NPU_VELA_CONFIG` empty):** the `--config` flag is omitted and
  Vela falls back to its **built-in generic system-config**. The build still
  succeeds, but the emitted command stream will **not** match the
  bench-validated E8 runtime (CMake prints a `WARNING` to that effect).
- **With the path supplied:** `gen_model.py` adds `--config <ini>` to the Vela
  invocation, so the command stream matches the silicon the hal_alif Ethos-U
  callback is built for.

A repository `.gitignore` rule (`ensemble_vela.ini`, `**/ensemble_vela.ini`)
guards against a stray copy — dropped next to an example by an operator — ever
being committed to the public repo.

## Run (bench)

This board has no app UART on USB, so the single `RESULT` line is read from the
RAM console (`ram_console_buf`) over SWD. Flash via **Flow D** (J-Link direct
MRAM) or the SETOOLS direct-ATOC path, then read back — see
`docs/aen-bench-bringup.md` and `scripts/bench/aen/`.
