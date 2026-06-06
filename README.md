# Alp SDK — Unification SDK for E1M Edge AI Modules

> Write once, run on any E1M module.

> ⚠️ **[UNTESTED] — `v0.6` status: paper-correct, no real-silicon verification yet.**
>
> Every chip driver, library binding, peripheral wrapper, and
> example app in this repo builds clean on `native_sim/native/64`
> and passes its NULL-arg-guard ZTEST.  **Nothing has been brought
> up on real silicon yet.**  Treat register addresses, timing
> values, lifecycle sequencing, and per-SoM accelerator wiring as
> *paper-correct only* until the v1.0 HiL verification sweep
> lands.  Per-driver verification status is recorded in
> `metadata/chips/<name>.yaml`'s `verification:` block and as
> `@par Verification status: [UNTESTED]` Doxygen tags on every
> public header.  Customers shipping production firmware should
> assume nothing in here has been silicon-validated and budget
> their own bring-up time accordingly.  Verification rolls out
> per-SKU + per-chip from v0.6 onward.

The **Alp SDK** is the unification software layer for Alp Lab edge AI
modules built on the **E1M open-standard form factor**.  It provides
application developers a single C/C++ API that works across every
E1M-* SoM variant — present and future — by wrapping each vendor's
SDK on top of ARM CMSIS.

Supported runtime topologies: **single-OS** (Zephyr / Yocto / bare-metal on a single core) and **heterogeneous** (Zephyr + Yocto + bare-metal coexisting on the same SoM, one declarative project).

**Where to go next:** rendered docs at
[**docs.alplab.ai/sdk/introduction**](https://docs.alplab.ai/sdk/introduction)
· questions + community at
[**community.alplab.ai**](https://community.alplab.ai/) ·
issues at
[**github.com/alplabai/alp-sdk/issues**](https://github.com/alplabai/alp-sdk/issues).

## Quickstart

```bash
# Install the CLI (once per clone)
pip install -e .

# Scaffold + run a hello-world on native_sim — no hardware needed
alp init my-app
cd my-app
alp run
```

`alp init` walks you through SoM SKU + board preset + starter peripherals interactively, or accepts `--som`, `--preset`, `--peripherals` flags for CI. `alp run` builds for `native_sim` by default and prints the app's stdout straight through; pass `--board <name>` for a real-hardware build (`--flash` to chain flash).

`alp validate board.yaml` runs the diagnostic-rich validator standalone — try it on a fixture under `tests/fixtures/board_yaml_bad/` to see the format.

## Two consumer paths

The SDK supports both flows equally — pick whichever fits.

- **Standalone / hand-written firmware.**  Write a Zephyr (or Yocto,
  or bare-metal) app against `<alp/...>` headers directly.  Pick
  instance IDs by hand from `<alp/e1m_pinout.h>` — `E1M_I2C0`,
  `E1M_PWM3`, etc. — and your app is portable across every
  E1M-conformant SoM.  Capability validation runs at runtime in
  `*_open`; `alp_last_error()` tells you why an open failed.
- **alp-studio codegen.**  The
  [studio](https://github.com/alplabai/alp-studio) reads the SoM
  preset stored in this repo's `metadata/e1m_modules/<SKU>.yaml`,
  runs the pin allocator using that preset's `pad_routes:` block,
  and emits C that calls the same `<alp/...>` API the
  hand-written path uses.  alp-sdk is the source of truth for
  per-SoM hardware data; alp-studio consumes it.  Pin allocation
  correctness comes for free.

The standalone path is **not** a studio escape hatch — it's a
first-class consumer.  Anything the studio can emit, a developer
should be able to write by hand.

## Portability promise — swap-and-run within a family

Change `som.sku:` in `board.yaml`, rebuild, ship — **within a SoM
family**.  Empirically proven across all 7 E1M SKUs (AEN301..801 +
NX9101) and all 4 E1M-X SKUs (V2N101, V2N102, V2M101, V2M102) for
the canonical portable examples; matrix at
[`docs/portability-matrix.md`](docs/portability-matrix.md).

Cross-form-factor portability between E1M and E1M-X is intentionally
NOT a goal — they are separate product lines with separate
`<alp/*_pinout.h>` namespaces.

- [`docs/portability.md`](docs/portability.md) — customer cookbook
  with the swap-test recipe, dual-namespace decision tree, the
  runtime-detection ladder pattern, and worked examples
  (AEN701 → AEN801, V2N101 → V2M101).
- [`docs/portability-matrix.md`](docs/portability-matrix.md) — the
  empirical guarantee (21/21 E1M + 12/12 E1M-X cells green; all 5
  Phase B gaps resolved).
- [ADR 0011](docs/adr/0011-intra-family-portability.md) —
  architectural decision: load-bearing intra-family scope,
  alternatives rejected (single namespace, lowest-common-denominator
  API, per-SoM custom APIs).
- [`docs/porting-new-som.md`](docs/porting-new-som.md) — 30-minute
  guide to adding a new SKU.

## Firmware engineers start here

Writing Zephyr / bare-metal C against an E1M-X module?
[**docs/firmware-quickstart.md**](docs/firmware-quickstart.md) is
the targeted walk-through: per-SoM choices, idiomatic patterns
for the on-module chips (PMICs, RTC, Wi-Fi/BT, Ethernet PHYs, DEEPX),
and pointers into the rest of the doc tree.

Pick your SoM and start with its one-pager:

| SoM family       | One-pager                                                   | Bring-up doc                                                | Reference examples                              |
|------------------|-------------------------------------------------------------|-------------------------------------------------------------|-------------------------------------------------|
| E1M-AEN          | [`docs/soms/aen.md`](docs/soms/aen.md)                      | [`docs/getting-started.md`](docs/getting-started.md) §4..5  | `examples/peripheral-io/gpio-button-led`, `i2c-scanner`, `rtc-clock`, `hello-world` |
| E1M-X V2N        | [`docs/soms/v2n.md`](docs/soms/v2n.md)                      | [`docs/bring-up-v2n.md`](docs/bring-up-v2n.md)              | `examples/v2n/v2n-gd32-bridge-ping`, `v2n-board-id-readout`, `v2n-ethernet-dual`, `dac-waveform` |
| E1M-X V2N-M1     | [`docs/soms/v2n-m1.md`](docs/soms/v2n-m1.md)                | [`docs/bring-up-v2n-m1.md`](docs/bring-up-v2n-m1.md)        | DEEPX bring-up delta on top of V2N             |
| E1M-N93 (i.MX93) | [`docs/soms/imx93.md`](docs/soms/imx93.md)                  | [`docs/getting-started.md`](docs/getting-started.md) §4..5  | same cross-family examples as AEN              |

**Peripheral tutorial set** (2026-05-18 — vendor-SDK-style basics):
`hello-world`, `uart-hello-world`, `i2c-master`, `i2c-slave`,
`spi-master`, `spi-slave`, `dac-waveform`, `timer-periodic-interrupt`
— each is a six-file teaching artifact with ~50 % comment density.
Full index in [`examples/README.md`](examples/README.md).

New to the terminology?  [**docs/glossary.md**](docs/glossary.md)
defines every term used across the SDK + module docs.  Stuck on
an error?  [**docs/troubleshooting.md**](docs/troubleshooting.md)
indexes the common ones with fixes.

## 30-second quick start

A v0.6 project is **one declarative file** plus per-core app
directories.  Drop a `board.yaml` at your app root:

```yaml
som:
  sku: E1M-V2N101      # your MPN -- the SDK ships a preset for every released MPN
  hw_rev: r1

preset: e1m-x-evk      # or write your board out inline -- see docs/board-config.md

cores:
  a55_cluster:
    app: ./linux                   # os: omitted -- A-cores default to yocto per topology
    image: alp-image-edge
    peripherals: [ethernet, usb, emmc]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    app: ./m33                     # os: omitted -- M-cores default to zephyr per topology
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
```

Each entry under `cores:` maps one on-die programmable core to a
runtime (`yocto`, `zephyr`, `baremetal`, or `off`) plus its app
slice.  The loader (`scripts/alp_project.py`) fans out per-core:
each Zephyr slice gets a Kconfig fragment layered onto its own
`prj.conf` via `OVERLAY_CONFIG`; each Yocto slice gets a
`local.conf` snippet consumed by bitbake.  Inside each
`cores.<id>` block every field except `os:` + `app:` is optional;
the [`gpio-button-led` example](examples/peripheral-io/gpio-button-led/) for
instance skips `peripherals:` entirely and uses `populated:
button_led: true` to pull the chip driver in.

For a **custom board** (not the EVK / X-EVK), drop the `preset:`
line and describe the board inline:

```yaml
name: my-sensor-board
populated:
  bmi323:  true
  ssd1306: true
e1m_routes:
  gpio:
    - { e1m: E1M_GPIO_IO15, macro: PIN_BMI323_INT1, doc: "BMI323 INT1" }
  buses:
    - { e1m: E1M_I2C0, macro: I2C_BUS_SENSORS }
  adc:
    - { e1m: E1M_ADC0, macro: ADC_VBAT_SENSE, doc: "Battery 4:1 divider" }
```

The `e1m_routes:` block covers all eight E1M peripheral classes
(gpio / buses / pwm / adc / dac / i2s / can / qenc) and per-entry
electrical facts (`active_low`, `pull: up|down|none`,
`debounce_ms`).  `scripts/gen_board_header.py` emits the matching
`include/alp/boards/alp_<name>_routes.h` automatically.

The same `board.yaml` also covers build-system knobs customers used
to hand-edit:

| Block | What it replaces |
|---|---|
| `cores.<id>.memory: { stack_kib, heap_kib, isr_stack_kib }` | `CONFIG_MAIN_STACK_SIZE` / `_HEAP_MEM_POOL_SIZE` / `_ISR_STACK_SIZE` in prj.conf |
| `cores.<id>.power: { sleep_mode, wakeup_sources }` | `CONFIG_PM` + `CONFIG_PM_DEVICE_WAKE_*` |
| `diagnostics.modules: { <name>: <level> }` | Per-module `CONFIG_<MOD>_LOG_LEVEL_*` |
| `boot: { method, signing, slots, swap_algorithm }` | Hand-edited `sysbuild.conf` |
| `ota: { provider, server, signing_key, ... }` | Hand-edited Mender `local.conf` |
| `storage:` + `security.psa:` | DTS partitions + TF-M sysbuild (v0.6 emit) |
| `pins: [{e1m, macro, doc}]` | None — surfaces the subset of pads each project actually uses |

See [`docs/board-config.md`](docs/board-config.md) for the full
schema reference and
[`docs/heterogeneous-builds.md`](docs/heterogeneous-builds.md) for
the dual-app project walk-through.

### What runs where

| SoM family | A-class cores | M-class cores | Heterogeneous? |
|---|---|---|---|
| E1M-AEN E3/E4 | — | M55-HP, M55-HE (both Zephyr) | No — RTOS-only silicon |
| E1M-AEN E5..E8 | A32 cluster (Yocto) | M55-HP, M55-HE (Zephyr) | Yes |
| E1M-X V2N / V2N-M1 | A55 cluster (Yocto) | M33-SM (Zephyr) | Yes |
| E1M-N93 (iMX93) | A55 cluster (Yocto) | M33 (Zephyr) | Yes |

A bare `som: { sku: <MPN> }` produces a working dual-image build
for every heterogeneous SoM — the per-core OS defaults come from
the SoM preset's `topology:` block.  Customers override on a
per-core basis via the project's `cores:` block.

Want a GUI?  Install the [VS Code extension](https://github.com/alplabai/alp-sdk-vscode) — schema-aware
editing, a configurator panel with dropdowns for every released MPN
and board, one-keypress "Generate all" for the four emit modes,
inline validator diagnostics in the Problems panel, west wrappers.

## Development hosts

First-class on **Linux**, **macOS**, and **Windows 11 / 10** (native
PowerShell or WSL2).  The Zephyr-on-M-class developer workflow is
fully cross-platform; only Yocto host builds require Linux (or WSL2)
by upstream `bitbake` / OE-core constraint.  Codified in
[ADR 0012](docs/adr/0012-cross-platform-developer-host.md).

- [`docs/cross-platform-setup.md`](docs/cross-platform-setup.md) —
  per-OS quickstart (Linux + macOS + Windows native + WSL2),
  verification walkthrough, known gotchas (MAX_PATH, AV, CRLF,
  Gatekeeper, serial device naming).
- `.gitattributes` pins line endings to LF so a Windows checkout
  and a Linux pull see identical bytes.
- VS Code config ships in `.vscode/`; install the
  [`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)
  extension for schema-aware `board.yaml` editing.
- `scripts/check_cross_platform.py` lints docs + scripts for
  Linux-only idioms; CI matrix scaffolding at
  `.github/workflows/cross-platform-zephyr.yml` runs the Python +
  loader smoke tests on Ubuntu (strict), macOS, and Windows
  (continue-on-error while runners prove out).
- See [`docs/getting-started.md`](docs/getting-started.md) for
  per-host setup notes.

## Status

**v0.6 ramp — paper-correct, pre-HIL** — recorded in
[`metadata/sdk_version.yaml`](metadata/sdk_version.yaml).  Surface
landed; runtime implementations fill in across point releases.  Code
merged ≠ verified — every claim is tracked in
[`docs/test-plan.md`](docs/test-plan.md), and a release does not tag
until its gating rows flip to ✅.

v0.6 lands heterogeneous OS orchestration — see ADR 0010 + [`docs/heterogeneous-builds.md`](docs/heterogeneous-builds.md).

**Backlog** (cherry-picked into future tags as items land — no per-version commitments; full list in [`VERSIONS.md`](VERSIONS.md)):

- AEN family + V2N101 + V2M101 silicon-verified via self-hosted lab HiL.
- `<alp/mproc.h>` shmem + hwsem implementations land on Zephyr backend.
- `<alp/power.h>` surface fleshed out for industrial / always-on use cases.
- Concurrent multi-NPU dispatch (DRP-AI3 + DEEPX) proven on V2M101.
- Mender OTA E2E (signed artefact + swap + rollback) on V2N101 fleet of N≥3 boards.
- ABI snapshot frozen for v1.0 commitment + 4 vertical reference apps verified end-to-end on real silicon.
- Customer onboarding ≤30-day dry-run passes.
- v1.0.0 cut after first customer pilot deployment completes.

*Deferred indefinitely past v1.0:* Ubuntu backend (`cores.<id>.os: ubuntu`), NXP NX9101 silicon enablement, FreeRTOS / ThreadX / NuttX backends.

- Doc navigation hub: [`docs/README.md`](docs/README.md).
- Roadmap: [`VERSIONS.md`](VERSIONS.md).
- What changed when: `CHANGELOG.md` (in the repo root).
- Per-(library × OS × SoM) status: [`docs/os-support-matrix.md`](docs/os-support-matrix.md).
- Architecture decisions: [`docs/adr/`](docs/adr/) (12 ADRs, latest:
  0011 intra-family portability + 0012 cross-platform host).

## Test it from scratch

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk
bash scripts/bootstrap.sh                    # west workspace + Python + apt hints
export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
bash scripts/test-all.sh                     # Yocto ctest + twister + format + Doxygen
```

A green run proves every `<alp/...>` surface compiles + links + passes
its failure-path tests across both Yocto and Zephyr backends.  Full
coverage map: [`docs/testing.md`](docs/testing.md).  Real-hardware
verification (`⏳`/`🟡`/`✅` rows) lives in
[`docs/test-plan.md`](docs/test-plan.md).

## The stack

### AI framework (on-device)

- **TFLite Micro** dispatched to silicon-specific NPU back-ends:
  - **Arm Ethos-U** — Alif Ensemble (AEN family; **U55** on every E3..E8 SKU, **U85** on E4 / E6 / E8 — Transformer-capable) + NXP i.MX 93 (N93 family; **U65**)
  - **Renesas DRP-AI3** — RZ/V2N (V2N family); supports YOLO v5 / v8 detection on top of classification + segmentation models.
  - **DEEPX DX-M1** — V2N + DX-M1 (V2M family); ONNX → DXNN compiler, model-family agnostic; first-class support for YOLO v5 / v8 / NAS detection backbones.
  - **CPU** — reference-kernel fallback on any target
- **Portable model pipeline (`.alpmodel`)** — `alp model build` compiles a source model for **every** NPU back-end the SoM declares into one **fat multi-backend `.alpmodel`** package (CBOR manifest + per-backend blobs + a capability `requires` envelope). At runtime **`alp_inference_open_alpmodel()`** loads the package and a selection engine picks the matching blob (silicon ref + SRAM-fit + `preferred_backend` tiebreak; `ALP_ERR_NO_FIT` if none fits), then dispatches through the backend registry below. One model, portable across NPUs without source changes.
- Offline training (off-device) lives in TensorFlow / PyTorch.

### Dev tooling

- **`board.yaml` project config** — single source of truth, covering: board identity (`name` / `description` / `hw_rev`, or `preset:` for the EVKs), SoM SKU + `hw_rev`, per-core runtime + app + peripherals + libraries + inference + iot + `memory` (stack/heap) + `power` (sleep/wakeup), board-side `populated:` chip list + `e1m_routes:` pad-to-macro routing (8 sections: gpio/buses/pwm/adc/dac/i2s/can/qenc, with `active_low` / `pull` / `debounce_ms`), used-pad subset `pins:`, cross-core `ipc:`, `boot:` (MCUboot config → sysbuild overlay), `ota:` (Mender config → Yocto local.conf), `storage:` (flash partitions), `security.psa:` (TF-M + persistent key slots), and `diagnostics:` with per-module log-level overrides
- **`scripts/alp_project.py`** — emits Zephyr Kconfig fragments, plain-CMake `-D` flags, Yocto `local.conf` snippets, DTS overlays, or the `<alp_hw_info_build.h>` companion header
- **`scripts/validate_board_yaml.py`** — customer-side linter (exit 0 / 1 schema / 2 missing-preset / 3 hw_rev incompatible)
- **`scripts/program_eeprom.py`** — packs board.yaml + serial + mfg date into the 128-byte EEPROM manifest for production-test programming
- **VS Code extension** ([`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)) — schema-aware `board.yaml` editor, GUI configurator, west wrappers, per-OS bootstrap, inline validator diagnostics

### Alp SDK (`<alp/...>`)

| Group | Headers + chip drivers |
|---|---|
| Peripherals | `peripheral.h` (GPIO/I²C/SPI/UART), `pwm.h`, `adc.h`, `counter.h`, `i2s.h`, `can.h`, `rtc.h`, `wdt.h`, `usb.h` |
| Audio / camera / display | `audio.h` (PDM in + I²S out), `camera.h`, `gui.h` (LVGL), `display.h` (panels) — chip drivers: SSD1306, SSD1331, ST7789, OV5640, CAM_MUX, TAS2563, PDM mic |
| Connectivity & security | `iot.h` (Wi-Fi/MQTT), `ble.h` (BLE 5.4), `security.h` (MbedTLS PSA Crypto), `storage.h` (LittleFS), OPTIGA Trust M chip driver |
| DSP / graphics / power | `dsp.h` (FFT / FAC / IIR chain), `tmu.h` (trig-/math-unit offload), `gpu2d.h` (2D blit/fill), `power.h` (sleep + wake sources) — HW-accelerated where the SoC provides it, SW fallback (CMSIS-DSP / libm / Zephyr PM) otherwise |
| Inference dispatcher | `inference.h` — portable models load via **`alp_inference_open_alpmodel()`** (the `.alpmodel` package + selection engine — see *AI framework* above), which dispatches to the registry-backed backend selector + tensor-arena management.  Backends registered today: `tflm` (CPU reference kernels, portable), `ethos_u_aen` (Ethos-U on Alif Ensemble — U55 every SKU, U85 on E4/E6/E8 Transformer-capable), `ethos_u_n93` (Ethos-U U65 on i.MX 93), `drpai_v2n_stub` (DRP-AI3 on RZ/V2N — tracked by issue #58), `deepx_dxm1_stub` (DEEPX DX-M1 — tracked by issue #59), `sw_fallback` (NOSUPPORT floor).  Selector picks the highest-priority backend matching the SoM's silicon ref; exact match beats `*` wildcard at equal priority. |
| Multi-proc / IPC | `mproc.h` (mailbox + shared memory + hardware semaphore) + `rpc.h` (framed RPC over RPMsg / OpenAMP) — the heterogeneous Zephyr↔Yocto path |
| Hardware info | `hw_info.h` — 128-byte EEPROM manifest + BOARD_ID ADC + `assert_matches_build()` |
| Vendor escape hatches | `ext/<vendor>/…` — Alif / Renesas / NXP / DEEPX surfaces for capabilities the portable `<alp/*>` API can't express (camera, inference, ADC, storage, power) |
| Chip drivers | **80+** under `chips/` — LSM6DSO, BMI323, ICM-42670, BMP581, INA236, TMP112, RV-3028-C7, 24C128, CC3501E, TCAL9538, button-LED helper, … |
| User libraries (via `libraries:` in board.yaml) | ETL · fmt · nlohmann_json · doctest · LVGL · MbedTLS · CMSIS-DSP · LittleFS |

### OS backend

Bare-metal · Yocto · Zephyr.  Selected per-core in `board.yaml`'s `cores:` block.

### Vendor SDK

Alif Ensemble · Renesas RZ/V2N · NXP i.MX 93 · DEEPX DX-M1.

### HW + HAL

E1M (35×35 mm) and E1M-X (45×65 mm) SoMs · E1M-EVK and E1M-X-EVK reference boards · vendor HALs.

```text
  ┌─────────────────────────────────────────────────────────────────────────────────────────────┐
  │ E1M™ — Scalable AI Smarter Edge                                                   ⚡ Alp Lab │
  └─────────────────────────────────────────────────────────────────────────────────────────────┘

  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ AI Models &   │ ─► │  Train (off-device):  TensorFlow · PyTorch  →  .tflite / .onnx         │
  │ Pipeline      │    │                                                                        │
  │               │    │  Compile (host):  alp model build  →  one fat .alpmodel package        │
  │               │    │     per-backend blobs:  Vela (Ethos-U) · DRP-AI · dxcom · CPU/TFLM     │
  │               │    │                                                                        │
  │               │    │  Model families:  classification · detection (YOLO v5/v8) ·            │
  │               │    │                   segmentation · keyword-spotting · pose               │
  │               │    │                                                                        │
  │               │    │  →  the .alpmodel runs at RUNTIME via the SDK's Inference block ↓      │
  └───────────────┘    └────────────────────────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────────────────────────┐
  │ Dev Tooling   │ ─► │  board.yaml · alp_project.py (per-core emit) · alp_orchestrate.py      │
  │ (v0.6)        │    │  west alp-build / alp-image / alp-flash / alp-clean                    │
  │               │    │  validate_board_yaml.py · program_eeprom.py · VS Code extension        │
  │               │    │  alp model build  →  .alpmodel   (the model-compile front-end)         │
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
  │               │    │  ─ LVGL · GPU2D/Dave2D   ─ BOARD_ID ADC         ─ <alp/tmu.h> · power  │
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

See [`docs/architecture.md`](docs/architecture.md) for the per-library
design, [`docs/board-config.md`](docs/board-config.md) for the
`board.yaml` schema reference, and
[`docs/zephyr-version-policy.md`](docs/zephyr-version-policy.md) for
how we pin Zephyr LTS + when bumps drive new alp-sdk releases.

## Public API

All consumer-facing headers live under `include/alp/`:

| Header               | Library                                    |
|----------------------|--------------------------------------------|
| `alp/peripheral.h`   | I²C, SPI, GPIO, UART                       |
| `alp/pwm.h` / `adc.h` / `counter.h` / `i2s.h` / `can.h` / `rtc.h` / `wdt.h` / `usb.h` | one peripheral class per header |
| `alp/camera.h` / `gui.h` / `display.h` | camera · LVGL re-export · display-panel driver |
| `alp/audio.h`        | PDM in / I²S out (+ smart-amp codecs, e.g. TAS2563) |
| `alp/iot.h` / `ble.h` | Wi-Fi station + MQTT · BLE 5.4 peripheral + central |
| `alp/security.h`     | MbedTLS PSA Crypto (hash / AEAD / TRNG)    |
| `alp/storage.h`      | Block + filesystem storage (LittleFS)      |
| `alp/inference.h` / `backend.h` | Inference dispatcher + the backend-registry seam (TFLM / Ethos-U / DRP-AI3 / DEEPX) |
| `alp/dsp.h` / `tmu.h` | DSP chain (FFT / FAC / IIR) + trig-/math-unit offload — HW-accelerated where present, SW fallback (CMSIS-DSP / libm) otherwise |
| `alp/gpu2d.h`        | Portable 2D-GPU blit/fill shim (Dave2D / GPU2D; SW fallback) |
| `alp/power.h`        | Low-power: sleep modes + wake-source management |
| `alp/mproc.h` / `rpc.h` | Heterogeneous IPC — mailbox / shared mem / hwsem + framed RPC over RPMsg (OpenAMP) |
| `alp/hw_info.h`      | EEPROM manifest + BOARD_ID ADC             |
| `alp/soc_caps.h`     | (generated) active-SoC capability constants |
| `alp/e1m_pinout.h` / `e1m_x_pinout.h` | E1M + E1M-X instance IDs + portability bounds (separate namespaces) |
| `alp/ext/<vendor>/…`  | Vendor escape-hatch extensions (Alif / Renesas / NXP / DEEPX) for capabilities the portable API can't express |
| `alp/boards/<board>.h` | Board-feature names (e.g. EVK pin map) |
| `chips/<part>/`      | **80+** chip drivers, opt-in via `board.yaml` `populated:` |

Per-row implementation status (which backend, which OS, HW-verified
vs. code-merged-pending) lives in
[`docs/os-support-matrix.md`](docs/os-support-matrix.md) and
[`docs/test-plan.md`](docs/test-plan.md) -- single sources of truth,
not duplicated here.

## Supported hardware

The SDK targets three SoM **families**.  Within each family every SKU
shares the same E1M routing and the same vendor HAL — they differ
only in SoC variant and memory tier — so a single backend covers the
whole family.

| Family             | Form factor       | SKUs                                                                                  | Primary silicon                                              | AI throughput | OS targets         |
|--------------------|-------------------|---------------------------------------------------------------------------------------|--------------------------------------------------------------|---------------|--------------------|
| **E1M-AEN**        | E1M (35×35 mm)    | `E1M-AEN301`, `E1M-AEN401`, `E1M-AEN501`, `E1M-AEN601`, `E1M-AEN701`, `E1M-AEN801`    | Alif Semiconductor *Ensemble* E3–E8 (Cortex-M55 + optional Cortex-A32 + Ethos-U55, plus Ethos-U85 on E4 / E6 / E8) | up to ~1024 GOPS | Zephyr · bare-metal |
| **E1M-X V2N**      | E1M-X (45×65 mm)  | `E1M-V2N101`, `E1M-V2N102`                                                            | Renesas RZ/V2N (4× Cortex-A55 + Cortex-M33 + DRP-AI3)        | 4 TOPS        | Yocto              |
| **E1M-X V2N-M1**   | E1M-X (45×65 mm)  | `E1M-V2M101`, `E1M-V2M102`                                                            | Renesas RZ/V2N + DEEPX DX-M1                                 | 4 + 25 TOPS   | Yocto              |
| **E1M-i.MX93**     | E1M (35×35 mm)    | TBD                                                                                   | NXP i.MX 93 (2× Cortex-A55 + Cortex-M33 + Ethos-U65)         | ~0.5 TOPS     | Yocto + Zephyr     |

All modules use the **E1M open-standard form factor**.  HW pinout and
mechanical specification live in
[`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec) — pinned
to **v1.1** for this revision of the SDK.

Evaluation kits: **E1M EVK** (UG-E1M-001) for the 35 × 35 mm
families, **E1M-X EVK** for the 45 × 65 mm families.  Per-EVK pinout
+ camera/display options + sensors in [`docs/boards/`](docs/boards/).
Mandatory on-module components (Ethernet PHYs, Wi-Fi 6 + BLE 5.4
combo, CAN transceivers, V2N supervisory MCU) are documented in
[`docs/architecture.md`](docs/architecture.md) and normative in
[`e1m-spec` §6.5](https://github.com/alplabai/e1m-spec/blob/main/STANDARD.md#65-mandatory-on-module-components).

## Consuming the SDK

The SDK can be consumed two ways:

### As a Zephyr module (recommended for Zephyr projects)

Add to your application's `west.yml`:

```yaml
manifest:
  projects:
    - name: alp-sdk
      url: https://github.com/alplabai/alp-sdk
      revision: main        # pin to a tag (v0.6.0, etc.) once released; v0.6 is pre-release
      path: modules/lib/alp-sdk
```

Then in your `prj.conf`:

```
CONFIG_ALP_SDK=y
```

Application code includes `<alp/peripheral.h>` and friends.

### As a plain CMake `add_subdirectory()` (bare-metal, Yocto, host tests)

```cmake
add_subdirectory(third_party/alp-sdk)
target_link_libraries(my_app PRIVATE alp::sdk)
```

`ALP_OS` selects the backend (`zephyr`, `baremetal`, or `yocto`).  Under
Zephyr it auto-detects `ZEPHYR_BASE`; otherwise it defaults to
`baremetal`.

## Build

```bash
# Plain CMake (host smoke test)
cmake -B build -DALP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure

# Zephyr (heterogeneous slice, v0.6 pre-release flow)
west init -m https://github.com/alplabai/alp-sdk --mr main alp-ws
cd alp-ws && west update
west alp-build examples/multicore/rpmsg-v2n
```

## Repository layout

```
alp-sdk/
├── include/alp/     # PUBLIC headers (the consumer surface)
├── src/             # common/ + zephyr/ + baremetal/ + yocto/ backends
├── chips/           # 80+ opt-in chip drivers
├── vendors/         # per-SoM HAL bindings (alif, renesas-rzv2n, nxp-imx93, deepx-dxm1)
├── metadata/        # schemas, templates, e1m_modules/E1M-<MPN>.yaml, boards/, socs/
├── scripts/         # board.yaml loader, validators, soc_caps + ABI generators, EEPROM packer
├── examples/        # reference apps (cross-family + examples/aen/ + examples/v2n/)
├── docs/            # architecture, board-config, ADRs, test-plan, …
├── tests/           # smoke + Twister + fuzz + bench + scripts
├── meta-alp-sdk/    # Yocto layer + machine confs
├── firmware/        # cc3501e/ + gd32-bridge/ on-module-MCU firmware
├── zephyr/          # Zephyr-module entry: Kconfig, module.yml, dts/bindings/, sysbuild/aen/
├── (build/)         # local build outputs — gitignored.  Yocto, Zephyr, and plain-CMake all land here (e.g. build/yocto-2b/, build/zephyr/, build/<example>/).
└── west.yml         # Zephyr manifest
```

See [`docs/porting-new-som.md`](docs/porting-new-som.md) for adding a
new E1M variant and [`docs/board-config.md`](docs/board-config.md) for
the `board.yaml` schema reference.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

Copyright 2026 Alp Lab AB.
