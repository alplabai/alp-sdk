# Alp SDK — Unification SDK for E1M Edge AI Modules

> Write once, run on any E1M module.

[![CI](https://github.com/alplabai/alp-sdk/actions/workflows/pr-twister.yml/badge.svg?branch=main)](https://github.com/alplabai/alp-sdk/actions/workflows/pr-twister.yml)
[![Release](https://img.shields.io/github/v/release/alplabai/alp-sdk)](https://github.com/alplabai/alp-sdk/releases)
[![License](https://img.shields.io/github/license/alplabai/alp-sdk)](LICENSE)
[![Zephyr](https://img.shields.io/badge/Zephyr-v4.4.1-blue)](docs/zephyr-version-policy.md)

**Alp SDK** is the unification software layer for Alp Lab edge AI modules
built on the **E1M open-standard form factor**. It gives you one C/C++ API
— `<alp/...>` — that works across every E1M-\* SoM variant by wrapping each
vendor's SDK on top of ARM CMSIS. Change `som.sku:` in a project's
`board.yaml`, rebuild, ship — within a SoM family, no source changes.

> [!WARNING]
> **Partially silicon-verified.** Every chip driver, peripheral wrapper, and
> example builds clean and passes CI on `native_sim`. Two SoM families carry
> real-silicon evidence today: **E1M-X V2N** (GD32-bridge stack, verified
> v0.6) and **E1M-AEN801** (peripheral matrix + NPU inference + CC3501E
> bridge, verified v0.8). The rest (i.MX 93, V2M/DEEPX, AEN301/401/501/601/701)
> remain pre-silicon. Per-feature status: [`docs/test-plan.md`](docs/test-plan.md);
> full caveats: [Status](README.md#status) below.

Rendered docs: [**docs.alplab.ai/sdk/introduction**](https://docs.alplab.ai/sdk/introduction) ·
community: [**community.alplab.ai**](https://community.alplab.ai/) ·
issues: [**github.com/alplabai/alp-sdk/issues**](https://github.com/alplabai/alp-sdk/issues)

## Quickstart

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk

# Install tan, the standalone build CLI (separate repo: alplabai/tan-cli).
# Linux/macOS/WSL2:
curl -fsSL https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh | sh
# Windows PowerShell: irm https://raw.githubusercontent.com/alplabai/tan-cli/main/install.ps1 | iex
# Open a new terminal if `tan` isn't immediately on PATH.

tan bootstrap --sdk-root "$PWD"    # one-time: west + Zephyr workspace + Python deps.
                                    # Works natively on Linux, macOS, and Windows
                                    # (PowerShell) for the Zephyr-on-M path used below;
                                    # only the Yocto/A-class path needs WSL2 -- see
                                    # docs/cross-platform-setup.md.
tan doctor                         # sanity-check the host before building

# One-time: the arm-zephyr-eabi cross toolchain a real-SoM build needs --
# bootstrap does not install this for you. Run from the workspace tan
# bootstrap just created (one level above this checkout).
( cd .. && west sdk install --gnu-toolchains arm-zephyr-eabi --no-hosttools \
    --install-dir "$PWD/zephyr-sdk" )
export ZEPHYR_SDK_INSTALL_DIR="$PWD/../zephyr-sdk"

# Scaffold a sibling project and build it for its default target
# (E1M-AEN801). --sdk-root pins this checkout in my-app/.alp/sdk-path;
# `tan sdk install`/`switch` aren't ported yet, so pin explicitly for now.
tan init --name my-app --destination .. --sdk-root "$PWD"
cd ../my-app
tan build
```

Full walkthrough (real-hardware flashing, per-SoM options, editing in
VS Code): [`docs/getting-started.md`](docs/getting-started.md). `tan` is
released independently from [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli);
the full verb reference is [`docs/cli.md`](docs/cli.md).

## The stack

What the SDK gives you, from model to silicon. Each layer is a real
boundary: you can swap the SoM under it, or the OS slice inside it,
without rewriting the layer above.

```text
  ┌─────────────────────────────────────────────────────────────────────────────────────────────┐
  │ E1M™ — Scalable AI Smarter Edge                                                   ⚡ Alp Lab │
  └─────────────────────────────────────────────────────────────────────────────────────────────┘

  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ AI Models &   │ ─► │  Train (off-device):  TensorFlow · PyTorch  →  .tflite / .onnx         │
  │ Pipeline      │    │                                                                        │
  │               │    │  Compile (host):  tan model build  →  one fat .alpmodel package        │
  │               │    │     per-backend blobs:  Vela (Ethos-U) · DRP-AI · dxcom · CPU/TFLM     │
  │               │    │                                                                        │
  │               │    │  Model families:  classification · detection (YOLO v5/v8) ·            │
  │               │    │                   segmentation · keyword-spotting · pose               │
  │               │    │                                                                        │
  │               │    │  →  the .alpmodel runs at RUNTIME via the SDK's Inference block ↓      │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ Dev Tooling   │ ─► │  board.yaml → Python tan (in-process planner + executor)               │
  │               │    │  SDK reference emits: alp_project.py · alp_orchestrate                 │
  │               │    │  tan build / flash / image / size / clean                              │
  │               │    │  validate_board_yaml.py · program_eeprom.py · VS Code extension        │
  │               │    │  tan model build  →  .alpmodel   (the model-compile front-end)         │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ Alp SDK       │ ─► │  Peripherals             Audio                  Camera                 │
  │ <alp/*.h>     │    │  ─ GPIO / I²C / SPI      ─ PDM in (mics)        ─ OV5640               │
  │               │    │  ─ UART / PWM / ADC      ─ I²S out + amp        ─ CAM_MUX              │
  │               │    │  ─ CAN / RTC / WDT / USB ─ TAS2563                                     │
  │               │    │                                                                        │
  │               │    │  Inference  ──  the .alpmodel runtime (where on-device AI runs)        │
  │               │    │  ─ alp_inference_open_alpmodel()  loads the fat .alpmodel              │
  │               │    │  ─ selects the blob: silicon-ref + SRAM-fit + preferred_backend        │
  │               │    │  ─ dispatches →  Ethos-U · DRP-AI3 · DEEPX DX-M1 · CPU / TFLM          │
  │               │    │                                                                        │
  │               │    │  IoT / BLE               Security               Storage                │
  │               │    │  ─ Wi-Fi 6 · MQTT        ─ MbedTLS PSA Crypto   ─ LittleFS             │
  │               │    │  ─ BLE 5.4               ─ OPTIGA Trust M       ─ <alp/storage.h>      │
  │               │    │                                                                        │
  │               │    │  Display / GUI           HW Info                DSP / Power            │
  │               │    │  ─ SSD1306 / 1331        ─ EEPROM manifest      ─ alp_dsp_* FFT/FAC/IIR│
  │               │    │  ─ LVGL · GPU2D/Dave2D   ─ EEPROM hw_info      ─ <alp/tmu.h> · power  │
  │               │    │                                                                        │
  │               │    │  Heterogeneous IPC:  <alp/rpc.h> · <alp/system_ipc.h> · <alp/mproc.h>  │
  │               │    │     framed RPMsg/OpenAMP · auto endpoint IDs · mailbox/shmem/hwsem     │
  │               │    │  Vendor escape hatches:  <alp/ext/{alif, renesas, nxp, deepx}>         │
  │               │    │                                                                        │
  │               │    │  ── 80+ Tier-1 chip drivers + Tier-2 community repo:                   │
  │               │    │        lsm6dso, bmi323, bmp581, icm42670, ina236, tmp112,              │
  │               │    │        tcal9538, rv3028c7, 24c128, cc3501e, ssd13xx, …                 │
  │               │    │  ── User libraries (board.yaml libraries:):                            │
  │               │    │        ETL · fmt · nlohmann_json · doctest · LVGL · MbedTLS ·          │
  │               │    │        CMSIS-DSP · LittleFS                                            │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ OS            │ ─► │  Zephyr (M-class cores) · Yocto (A-class cores) · Bare-metal           │
  │ (per-core     │    │  heterogeneous = peers on the same SoM (per-core in cores:)            │
  │  slice)       │    │                                                                        │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ Vendor SDK    │ ─► │  Alif Ensemble (AEN) · Renesas RZ/V2N · NXP i.MX 93 · DEEPX DX-M1      │
  │               │    │  NPU runtimes dispatched into: Ethos-U/Vela · DRP-AI · DEEPX dx_rt     │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ HW + HAL      │ ─► │  E1M (35×35 mm) + E1M-X (45×65 mm) SoMs  ·  NPU silicon                │
  │               │    │  E1M-EVK / E1M-X-EVK reference boards  +  vendor HALs                  │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
```

## Two consumer paths

Both are first-class — pick whichever fits:

- **Standalone / hand-written firmware.** Write Zephyr, Yocto, or bare-metal
  C directly against `<alp/...>` headers, using instance IDs from
  `<alp/e1m_pinout.h>` (E1M) or `<alp/e1m_x_pinout.h>` (E1M-X). The
  standalone path is not a fallback — it's what the studio path also
  compiles down to.
- **alp-studio codegen.** `alplabai/alp-studio` (not a public GitHub repo)
  reads this repo's per-SoM presets (`metadata/e1m_modules/<SKU>.yaml`), runs
  its pin allocator, and emits the same `<alp/...>` calls you'd write by
  hand.

See [ADR 0001](docs/adr/0001-wrapper-on-top-of-zephyr.md) and
[ADR 0005](docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md).

## Portability

Swap-and-run is measured **within** a SoM family, against the generated
swap-test matrix: the 6 released E1M-AEN SKUs pass all three canonical
examples (18 / 21 E1M cells — the remaining 3 are `E1M-NX9101`, a
placeholder MPN whose only hw_rev is `status: tbd`, refused by the
hw_rev-buildable gate and so not yet buildable at all), and the 4 E1M-X
SKUs pass two of three (8 / 12 cells — `adc-voltmeter` fails on all
four). Matrix at
[`docs/portability-matrix.md`](docs/portability-matrix.md). Crossing
between E1M and E1M-X is intentionally out of scope: they're separate
product lines with separate pinout namespaces
([ADR 0011](docs/adr/0011-intra-family-portability.md)). Cookbook + worked
examples: [`docs/portability.md`](docs/portability.md).

## Supported hardware

| Family | Form factor | SKUs | Primary silicon | AI throughput | OS targets |
|---|---|---|---|---|---|
| **E1M-AEN** | E1M (35×35 mm) | `E1M-AEN301/401/501/601/701/801` | Alif Ensemble E3–E8 (Cortex-M55 + optional A32 + Ethos-U55, U85 on E4/E6/E8) | up to ~1024 GOPS | Zephyr · bare-metal |
| **E1M-X V2N** | E1M-X (45×65 mm) | `E1M-V2N101` | Renesas RZ/V2N (4× A55 + M33 + DRP-AI3) | 4 TOPS | Yocto (A55) · Zephyr (M33 system manager) |
| **E1M-X V2N** | E1M-X (45×65 mm) | `E1M-V2N102` | Renesas RZ/V2N (4× A55 + M33 + DRP-AI3) | 4 TOPS | Yocto (A55); Zephyr M33 tree not yet built |
| **E1M-X V2N-M1** | E1M-X (45×65 mm) | `E1M-V2M101` | Renesas RZ/V2N + DEEPX DX-M1 | 4 + 25 TOPS | Yocto (A55) · Zephyr (M33 system manager) |
| **E1M-X V2N-M1** | E1M-X (45×65 mm) | `E1M-V2M102` | Renesas RZ/V2N + DEEPX DX-M1 | 4 + 25 TOPS | Yocto (A55); Zephyr M33 tree not yet built |
| **E1M-i.MX93** | E1M (35×35 mm) | TBD | NXP i.MX 93 (2× A55 + M33 + Ethos-U65) | ~0.5 TOPS | Yocto · Zephyr |

All modules share the **E1M open-standard form factor** — pinout + mechanical
spec in [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec) (pinned
v1.1). Evaluation kits: **E1M EVK** and **E1M-X EVK**, per-EVK detail in
[`docs/boards/`](docs/boards/).

## Firmware engineers, start here

Pick your SoM's one-pager, bring-up guide, and reference examples in
[`docs/firmware-quickstart.md`](docs/firmware-quickstart.md). New to the
terms? [`docs/glossary.md`](docs/glossary.md). Stuck on an error?
[`docs/troubleshooting.md`](docs/troubleshooting.md).

## Using with VS Code

This repo's `.vscode/` config (extensions, tasks, `c_cpp_properties.json`) is
pre-wired for Zephyr-module + plain-CMake development — clone, open the
folder, accept the recommended extensions. For schema-aware `board.yaml`
editing, a GUI configurator, and `tan` wrappers, install the
[`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)
extension (a separate repo).

## Development hosts

First-class on **Linux**, **macOS**, and **Windows 11/10** (native
PowerShell or WSL2). Real-silicon Zephyr builds and `tan build` do **not**
work on Intel Macs — the pinned Zephyr SDK dropped its `macos-x86_64` host
build; `native_sim` still works there. Yocto builds need Linux or WSL2 by
upstream `bitbake` constraint. Per-OS quickstart + gotchas:
[`docs/cross-platform-setup.md`](docs/cross-platform-setup.md)
([ADR 0012](docs/adr/0012-cross-platform-developer-host.md)).

## Status

**Current ramp — paper-correct, mostly pre-HIL; partial silicon-verified
additions.** Code merged ≠ verified: every claim is tracked in
[`docs/test-plan.md`](docs/test-plan.md), and a release doesn't tag until
its gating rows flip to ✅. Treat register addresses, timing values, and
per-SoM accelerator wiring as paper-correct only until their test-plan row
flips. Silicon-verified today: the V2N GD32-bridge campaign (since v0.6)
and AEN801 (15/17 peripheral apps) + CC3501E (Wi-Fi/BLE, GPIO proxy) since
v0.8; breadth beyond these families remains pre-HIL. Per-driver status also
lives in `metadata/chips/<name>.yaml`'s `verification:` block and as
`@par Verification status: [UNTESTED]` Doxygen tags.

Backlog (cherry-picked as items land, no per-version commitments — full list
in [`VERSIONS.md`](VERSIONS.md)): AEN family + V2N101 + V2M101
silicon-verified via self-hosted HiL · `<alp/mproc.h>` shmem/hwsem on
Zephyr · `<alp/power.h>` fleshed out · concurrent multi-NPU dispatch on
V2M101 · Mender OTA E2E on a V2N101 fleet · ABI snapshot frozen for v1.0 ·
≤30-day customer onboarding dry-run · v1.0.0 after first customer pilot.
Deferred indefinitely past v1.0: the Ubuntu backend, NXP NX9101 silicon
enablement, FreeRTOS/ThreadX/NuttX backends.

- Doc navigation hub: [`docs/README.md`](docs/README.md)
- Per-(library × OS × SoM) status: [`docs/os-support-matrix.md`](docs/os-support-matrix.md)
- What changed when: [`CHANGELOG.md`](https://github.com/alplabai/alp-sdk/blob/HEAD/CHANGELOG.md)
- Architecture + design: [`docs/architecture.md`](docs/architecture.md);
  decision records: [`docs/adr/`](docs/adr/)
- Full local verification (`bash scripts/test-all.sh`) and what a green run
  proves: [`docs/testing.md`](docs/testing.md)
- Contributing: [`docs/contribution.md`](docs/contribution.md)

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

Copyright 2026 Alp Lab AB.
