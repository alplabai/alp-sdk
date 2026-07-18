<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-npu-inference-alif — matched-runtime Ethos-U85 inference (E1M-AEN801)

Runs a Vela-compiled int8 model end-to-end on the **E1M-AEN801** (Alif Ensemble
**E8**, Cortex-M55 RTSS-HE) Ethos-U85 (256-MAC): a fixture `tiny_int8.tflite`
flows through the Arm Apache-2.0 `InferenceProcess` wrapper → TFLM
`MicroInterpreter` (`AddEthosU()`) → `ethosu_invoke()` onto the NPU command
stream, and the app prints a single `RESULT PASS`/`FAIL` line.

This is the example whose silicon bring-up fixed the long-standing
`ethosu_invoke=1` residual. Two in-app **strong** weak-overrides are the fix
(both in `src/main.cpp`, both grounded — nothing re-authored):

1. `ethosu_address_remap = local_to_global()` — the CPU→NPU address translation
   the Arm core driver needs for any TCM-resident buffer. `local_to_global()` is
   hal_alif's (`soc_memory_map.h`); the overlay supplies the `global_base` props
   on the ITCM/DTCM nodes that it reads.
2. `ethosu_config_select()` returning MEM_ATTR index 0 (the **SRAM AXI port**) for
   the command stream and every region — because the Alif `Ethos_U85_SRAM_Only`
   memory mode places the model/arena/IO in SRAM0, reachable only over the SRAM
   port (the Arm default routes region 0 + the command stream to the EXT port and
   aborts).

> The model + tensor arena are DMA-visible to the NPU master and live in **SRAM0**
> (`@0x02000000`) with **`CONFIG_DCACHE=n`** — never DTCM. This is the same
> DMA-placement rule the Ethernet (`eth_dwmac`) bring-up established for every E8
> DMA master.

## Build

Silicon-only example (board target
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`). It needs four modules and the
Vela toolchain at build time:

```sh
west build -p always \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-npu-inference-alif \
  -- \
  -DEXTRA_ZEPHYR_MODULES="<alp-sdk>;<hal_alif>;<hal/ethos_u>;<lib/tflite-micro>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-npu-inference-alif/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay \
  -DAEN_NPU_VELA=<venv>/bin/vela \
  -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/.../ensemble_vela.ini
```

`-D` cache overrides (see `CMakeLists.txt` for defaults):

| Var | Purpose |
| --- | --- |
| `AEN_NPU_VELA` | Vela executable (default: `vela` on `PATH`). |
| `AEN_NPU_VELA_CONFIG` | **Alif-proprietary** `ensemble_vela.ini` — see *Licensing*. |
| `AEN_NPU_MODEL` | Input `.tflite` (default: the repo `tiny_int8.tflite` fixture). |
| `AEN_NPU_ACCEL` / `AEN_NPU_SYSTEM_CONFIG` / `AEN_NPU_MEMORY_MODE` | Vela target (`ethos-u85-256` / `Ethos_U85_SRAM_Only` / `Sram_Only`). |

`gen_model.py` Vela-compiles `AEN_NPU_MODEL` at configure time and emits
`model.h` (a `const` array in the FLASH/rodata region, `memcpy`'d into an
uninitialized SRAM0 buffer at boot — initialized `SRAM0` sections are **not**
init-copied). No model data is committed.

`CONFIG_ARM_ETHOS_U` is intentionally **off**: the stale hal_alif
`ethosu_callback.c` targets an older core-driver dcache signature and would
clash with the pinned `hal_ethos_u`. The strong remap comes from the app, the
correct-signature cache hooks from upstream `ethos_u_common.c`
(`CONFIG_ETHOS_U_DCACHE=y`).

## Run (bench)

This board has no app UART on USB, so the single `RESULT` line is read from the
RAM console (`ram_console_buf`) over SWD. Flash via **Flow D** (J-Link direct
MRAM) or the SETOOLS direct-ATOC path, then read back — see
`docs/aen-bench-bringup.md` and `scripts/bench/aen/`. Expected:

```
ethosu_wait(): Inference finished successfully
RESULT PASS
```

The fixture is an ITCM RAM-run image (≤256 KiB). **Real** models that overflow
ITCM once Vela'd (e.g. `person_detect`, ~263 KiB) link into the MRAM slot0 boot
path instead — see the MRAM-resident variant.

## Licensing

- `ethosu_utils/inference_process.{cpp,hpp}` — **Apache-2.0 (Arm)**, vendored
  Tier-2 with the provenance header intact.
- `ensemble_vela.ini` — **Alif-proprietary**, *not* redistributed by alp-sdk.
  Obtain it from **alp-sdk-internal** and pass it via `-DAEN_NPU_VELA_CONFIG`.
  Without it, Vela uses its built-in system-config and the emitted command
  stream will **not** match the bench-validated E8 runtime.
