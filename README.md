# ALP SDK — Unification SDK for E1M Edge AI Modules

> Write once, run on any E1M module.

> ⚠️ **[UNTESTED] — `v0.5` status: paper-correct, no real-silicon verification yet.**
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

The **ALP SDK** is the unification software layer for ALP Lab edge AI
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

## Two consumer paths

The SDK supports both flows equally — pick whichever fits.

- **Standalone / hand-written firmware.**  Write a Zephyr (or Yocto,
  or bare-metal) app against `<alp/...>` headers directly.  Pick
  instance IDs by hand from `<alp/e1m_pinout.h>` — `E1M_I2C0`,
  `E1M_PWM3`, etc. — and your app is portable across every
  E1M-conformant SoM.  Capability validation runs at runtime in
  `*_open`; `alp_last_error()` tells you why an open failed.
- **alp-studio codegen.**  The
  [studio](https://github.com/alplabai/alp-studio) reads block
  manifests, runs the pin allocator over the active SoM's
  manifest, and emits C that calls the same `<alp/...>` API the
  hand-written path uses.  Pin allocation correctness comes for
  free.

The standalone path is **not** a studio escape hatch — it's a
first-class consumer.  Anything the studio can emit, a developer
should be able to write by hand.

## Firmware engineers start here

Writing Zephyr / bare-metal C against an E1M-X module?
[**docs/firmware-quickstart.md**](docs/firmware-quickstart.md) is
the targeted walk-through: per-SoM choices, idiomatic patterns
for the on-module chips (PMICs, RTC, Wi-Fi/BT, Ethernet PHYs, DEEPX),
and pointers into the rest of the doc tree.

Pick your SoM and start with its one-pager:

| SoM family       | One-pager                                                   | Bring-up doc                                                | Reference examples                              |
|------------------|-------------------------------------------------------------|-------------------------------------------------------------|-------------------------------------------------|
| E1M-AEN          | [`docs/soms/aen.md`](docs/soms/aen.md)                      | [`docs/getting-started.md`](docs/getting-started.md) §4..5  | `examples/gpio-button-led`, `i2c-scanner`, `rtc-clock` |
| E1M-X V2N        | [`docs/soms/v2n.md`](docs/soms/v2n.md)                      | [`docs/bring-up-v2n.md`](docs/bring-up-v2n.md)              | `examples/v2n/v2n-gd32-bridge-ping`, `v2n-board-id-readout`, `v2n-ethernet-dual` |
| E1M-X V2N-M1     | [`docs/soms/v2n-m1.md`](docs/soms/v2n-m1.md)                | [`docs/bring-up-v2n-m1.md`](docs/bring-up-v2n-m1.md)        | DEEPX bring-up delta on top of V2N             |
| E1M-N93 (i.MX93) | [`docs/soms/imx93.md`](docs/soms/imx93.md)                  | [`docs/getting-started.md`](docs/getting-started.md) §4..5  | same cross-family examples as AEN              |

New to the terminology?  [**docs/glossary.md**](docs/glossary.md)
defines every term used across the SDK + module docs.  Stuck on
an error?  [**docs/troubleshooting.md**](docs/troubleshooting.md)
indexes the common ones with fixes.

## 30-second quick start

A v0.6 project is **one declarative file** plus per-core app
directories.  Drop a `board.yaml` at your app root:

```yaml
schema_version: 2

som:
  sku: E1M-V2N101      # your MPN -- the SDK ships a preset for every released MPN
  hw_rev: r1

carrier:
  name: E1M-X-EVK      # or your own custom carrier

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
the [`gpio-button-led` example](examples/gpio-button-led/) for
instance skips `peripherals:` entirely and uses
`carrier.populated.button_led: true` to pull the chip driver in.
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
and carrier, one-keypress "Generate all" for the four emit modes,
inline validator diagnostics in the Problems panel, west wrappers.

## Development hosts

First-class on **Linux**, **macOS**, and **Windows 11 / 10** (native
PowerShell or WSL2).  Line endings pinned to LF via `.gitattributes`
so a Windows checkout and a Linux pull see identical bytes.  VS Code
config ships in `.vscode/` (recommended extensions, build tasks,
debug profiles); install the
[`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)
extension for schema-aware `board.yaml` editing.  See
[`docs/getting-started.md`](docs/getting-started.md) for per-host
setup notes.

## Status

**v0.6 ramp — paper-correct, pre-HIL** — recorded in
[`metadata/sdk_version.yaml`](metadata/sdk_version.yaml).  Surface
landed; runtime implementations fill in across point releases.  Code
merged ≠ verified — every claim is tracked in
[`docs/test-plan.md`](docs/test-plan.md), and a release does not tag
until its gating rows flip to ✅.

v0.6 lands heterogeneous OS orchestration — see ADR 0010 + [`docs/heterogeneous-builds.md`](docs/heterogeneous-builds.md).

- Roadmap: [`VERSIONS.md`](VERSIONS.md).
- What changed when: [`CHANGELOG.md`](CHANGELOG.md).
- Per-(library × OS × SoM) status: [`docs/os-support-matrix.md`](docs/os-support-matrix.md).
- Architecture decisions: [`docs/adr/`](docs/adr/).

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
- Offline training (off-device) lives in TensorFlow / PyTorch.

### Dev tooling

- **`board.yaml` project config** — single source of truth: SoM SKU, carrier, OS, inference backend, libraries, peripherals, IoT toggles, diagnostics, `hw_rev`
- **`scripts/alp_project.py`** — emits Zephyr Kconfig fragments, plain-CMake `-D` flags, Yocto `local.conf` snippets, DTS overlays, or the `<alp_hw_info_build.h>` companion header
- **`scripts/validate_board_yaml.py`** — customer-side linter (exit 0 / 1 schema / 2 missing-preset / 3 hw_rev incompatible)
- **`scripts/program_eeprom.py`** — packs board.yaml + serial + mfg date into the 128-byte EEPROM manifest for production-test programming
- **VS Code extension** ([`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)) — schema-aware `board.yaml` editor, GUI configurator, west wrappers, per-OS bootstrap, inline validator diagnostics

### Alp SDK (`<alp/...>`)

| Group | Headers + chip drivers |
|---|---|
| Peripherals | `peripheral.h` (GPIO/I²C/SPI/UART), `pwm.h`, `adc.h`, `counter.h`, `i2s.h`, `can.h`, `rtc.h`, `wdt.h`, `usb.h` |
| Audio / camera / display | `audio.h` (PDM in + I²S out), `camera.h`, `gui.h` (LVGL) — chip drivers: SSD1306, SSD1331, OV5640, CAM_MUX, TAS2563, PDM mic |
| Connectivity & security | `iot.h` (Wi-Fi/MQTT), `ble.h` (BLE 5.4), `security.h` (MbedTLS PSA Crypto), OPTIGA Trust M chip driver |
| Inference dispatcher | `inference.h` — backend selector (auto / cpu / ethos_u / drpai / deepx_dx) + tensor-arena management.  `ethos_u` covers U55 (every Alif Ensemble SKU), U85 (E4 / E6 / E8 only -- Transformer-capable), and U65 (i.MX 93); the project loader picks per-NPU driver gates from the SoM's silicon ref. |
| Multi-proc / IPC | `mproc.h` — mailbox + shared memory + hardware semaphore |
| Hardware info | `hw_info.h` — 128-byte EEPROM manifest + BOARD_ID ADC + `assert_matches_build()` |
| Chip drivers | 20+ under `chips/` — LSM6DSO, BMI323, ICM-42670, BMP581, INA236, TMP112, RV-3028-C7, 24C128, CC3501E, TCAL9538, button-LED helper, … |
| User libraries (via `libraries:` in board.yaml) | ETL · fmt · nlohmann_json · doctest · LVGL · MbedTLS · CMSIS-DSP · LittleFS |

### OS backend

Bare-metal · Yocto · Zephyr.  Selected per-core in `board.yaml`'s `cores:` block.

### Vendor SDK

Alif Ensemble · Renesas RZ/V2N · NXP i.MX 93 · DEEPX DX-M1.

### HW + HAL

E1M (35×35 mm) and E1M-X (45×65 mm) SoMs · E1M-EVK and E1M-X-EVK reference carriers · vendor HALs.

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│  E1M™ — Scalable AI Smarter Edge                              ⚡ Alp Lab     │
└──────────────────────────────────────────────────────────────────────────────┘

  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │ AI Framework  │ ─► │  TFLM   →  Ethos-U   Alif AEN (U55 all E3..E8;     │
  │  (on-device)  │    │                       U85 on E4/E6/E8 — Transformer)│
  │               │    │                      NXP N93 (U65)                 │
  │               │    │         →  DRP-AI3   (Renesas V2N)                 │
  │               │    │         →  DEEPX DX-M1  (V2M family)               │
  │               │    │         →  CPU       (reference kernels)           │
  │               │    │  Model families: classification, detection (YOLO   │
  │               │    │    v5/v8 on DEEPX + DRP-AI3), segmentation,        │
  │               │    │    keyword-spotting, pose                          │
  │               │    │  · · · offline training only: TensorFlow, PyTorch  │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │ Dev Tooling   │ ─► │  board.yaml v2  ·  alp_project.py (per-core emit)  │
  │   (v0.6)      │    │  alp_orchestrate.py (fan-out + system-manifest)    │
  │               │    │  west alp-build / alp-image / alp-flash / alp-clean│
  │               │    │  validate_board_yaml.py  ·  VS Code extension      │
  │               │    │  program_eeprom.py  ·  per-OS dependency bootstrap │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │   Alp SDK     │ ─► │  Peripherals          Audio           Camera       │
  │  <alp/*.h>    │    │  ─ GPIO/I²C/SPI       ─ PDM in (mics) ─ OV5640     │
  │               │    │  ─ UART/PWM/ADC       ─ I²S out + amp ─ CAM_MUX    │
  │               │    │  ─ CAN/RTC/WDT/USB    ─ TAS2563                    │
  │               │    │                                                    │
  │               │    │  Inference            IoT / BLE       Security     │
  │               │    │  ─ dispatcher         ─ Wi-Fi 6       ─ MbedTLS    │
  │               │    │  ─ Ethos-U / DRP-AI   ─ MQTT          ─ PSA Crypto │
  │               │    │  ─ DEEPX / CPU        ─ BLE 5.4       ─ OPTIGA TM  │
  │               │    │                                                    │
  │               │    │  Display / GUI        HW Info         DSP / Power  │
  │               │    │  ─ SSD1306 / 1331     ─ EEPROM mfst   ─ alp_dsp_*  │
  │               │    │  ─ LVGL               ─ BOARD_ID ADC    chain (FFT │
  │               │    │  ─ GPU2D / Dave2D     ─ <alp/hw_info>   FAC, IIR)  │
  │               │    │                                       ─ alp/power  │
  │               │    │                                                    │
  │               │    │  Heterogeneous IPC   (v0.6 NEW)                    │
  │               │    │  ─ <alp/rpc.h>       framed RPMsg over OpenAMP     │
  │               │    │  ─ <alp/system_ipc.h> auto-generated endpoint IDs  │
  │               │    │  ─ <alp/mproc.h>     mailbox · shared mem · hwsem  │
  │               │    │                                                    │
  │               │    │  ─── 80 Tier 1 chip drivers + Tier 2 community repo│
  │               │    │       (lsm6dso, bmi323, bmp581,                    │
  │               │    │       icm42670, ina236, tmp112, tcal9538,          │
  │               │    │       rv3028c7, 24c128, cc3501e, ssd13xx, ...)     │
  │               │    │                                                    │
  │               │    │  ─── User libraries (board.yaml `libraries:`):     │
  │               │    │       ETL · fmt · nlohmann_json · doctest          │
  │               │    │       LVGL · MbedTLS · CMSIS-DSP · LittleFS        │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │  OS  (per-    │ ─► │   Zephyr             Yocto            Bare-metal   │
  │   core slice  │    │   M-class cores      A-class cores    no-RTOS      │
  │   in cores:)  │    │   (heterogeneous = peers on the same SoM)          │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │  Vendor SDK   │ ─► │   Alif Ensemble (AEN)   Renesas RZ/V2N             │
  │               │    │   NXP i.MX 93           DEEPX DX-M1                │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │      HW       │ ─► │   E1M (35×35) + E1M-X (45×65) SoMs                 │
  │   + HAL       │    │   E1M-EVK / E1M-X-EVK carriers + vendor HALs       │
  └───────────────┘    └────────────────────────────────────────────────────┘
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
| `alp/camera.h` / `gui.h`             | camera + LVGL re-export      |
| `alp/iot.h`          | Wi-Fi station + MQTT                       |
| `alp/audio.h`        | PDM in / I²S out                           |
| `alp/ble.h`          | BLE peripheral + central                   |
| `alp/security.h`     | MbedTLS PSA Crypto (hash / AEAD / TRNG)    |
| `alp/mproc.h`        | Multi-proc IPC (mailbox / shared mem / hwsem) |
| `alp/inference.h`    | Inference dispatcher (TFLM / Ethos-U / DRP-AI / DEEPX) |
| `alp/hw_info.h`      | EEPROM manifest + BOARD_ID ADC             |
| `alp/soc_caps.h`     | (generated) active-SoC capability constants |
| `alp/e1m_pinout.h`   | E1M-spec instance IDs + portability bounds |
| `alp/boards/<carrier>.h` | Carrier-feature names (e.g. EVK pin map) |
| `chips/<part>/`      | 20+ chip drivers, opt-in via `board.yaml` `carrier.populated:` |

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
| **E1M-X V2N-M1**   | E1M-X (45×65 mm)  | `E1M-V2M101`, `E1M-V2M102`                                                            | Renesas RZ/V2N + DeepX DX-M1                                 | 4 + 25 TOPS   | Yocto              |
| **E1M-i.MX93**     | E1M (35×35 mm)    | TBD                                                                                   | NXP i.MX 93 (2× Cortex-A55 + Cortex-M33 + Ethos-U65)         | ~0.5 TOPS     | Yocto              |

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
west alp-build examples/rpmsg-v2n
```

## Repository layout

```
alp-sdk/
├── include/alp/     # PUBLIC headers (the consumer surface)
├── src/             # common/ + zephyr/ + baremetal/ + yocto/ backends
├── chips/           # 20+ opt-in chip drivers
├── vendors/         # per-SoM HAL bindings (alif, renesas-rzv2n, nxp-imx93, deepx-dxm1)
├── metadata/        # schemas, templates, e1m_modules/E1M-<MPN>.yaml, carriers/, socs/
├── scripts/         # board.yaml loader, validators, soc_caps + ABI generators, EEPROM packer
├── examples/        # reference apps (cross-family + examples/aen/ + examples/v2n/)
├── docs/            # architecture, board-config, ADRs, test-plan, …
├── tests/           # smoke + Twister + fuzz + bench + scripts
├── yocto/meta-alp/  # Yocto layer + machine confs
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

Copyright 2026 ALP Lab AB.
