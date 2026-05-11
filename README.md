# ALP SDK — Unification SDK for E1M Edge AI Modules

> Write once, run on any E1M module.

The **ALP SDK** is the unification software layer for ALP Lab edge AI
modules built on the **E1M open-standard form factor**.  It provides
application developers a single C/C++ API that works across every
E1M-* SoM variant — present and future — by wrapping each vendor's
SDK on top of ARM CMSIS.

Supported OS targets: **Bare-metal · Zephyr RTOS · Yocto Linux**.

## Two consumer paths

The SDK supports both flows equally — pick whichever fits.

- **Standalone / hand-written firmware.**  Write a Zephyr (or Yocto,
  or bare-metal) app against `<alp/...>` headers directly.  Pick
  instance IDs by hand from `<alp/e1m_pinout.h>` — `ALP_E1M_I2C0`,
  `ALP_E1M_PWM3`, etc. — and your app is portable across every
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

## 30-second quick start

A v0.3 project is **one declarative file** plus an empty `prj.conf`.
Drop a `board.yaml` at your app root:

```yaml
schema_version: 1

som:
  sku: E1M-AEN701      # your MPN -- the SDK ships a preset for every released MPN

carrier:
  name: E1M-EVK        # or your own custom carrier

os: zephyr             # zephyr | yocto | baremetal

peripherals:           # what your app actually uses
  - i2c
  - pwm

diagnostics:
  log_level: info
```

The build picks it up automatically — `scripts/alp_project.py`
emits `build/generated/alp.conf` at CMake-configure time and Zephyr
layers it on top of `prj.conf` via `OVERLAY_CONFIG`.  See the
[`gpio-button-led` example](examples/gpio-button-led/) for the
complete wiring and [`docs/board-config.md`](docs/board-config.md)
for the full schema reference.

Want a GUI?  Install the [VS Code extension](vscode/) — schema-aware
editing, a configurator panel with dropdowns for every released MPN
and carrier, one-keypress "Generate all" for the four emit modes,
inline validator diagnostics in the Problems panel, west wrappers.

## Cross-platform development

The SDK is a first-class developer experience on **macOS**,
**Windows**, and **Linux**.  No platform-specific lock-in: the
toolchain (Zephyr / west / CMake / Python / GCC) ships for all
three, the public headers compile under every standard C99
toolchain, and CI artefacts are reproducible across hosts.

| Platform               | Status      | Setup notes                                              |
|------------------------|-------------|----------------------------------------------------------|
| **Linux** (Ubuntu/Fedora/Arch) | first-class | Zephyr's preferred host.  Native_sim runs at full speed. |
| **macOS** (Apple Silicon / Intel) | first-class | Native build; native_sim works.  Use `brew` for prerequisites. |
| **Windows 11 / 10**    | first-class — two paths | (a) **Native PowerShell** with Zephyr SDK + Python (Microsoft Store); (b) **WSL2 + Ubuntu** for Linux-identical workflows. |

Path detail for Windows users:

- *Native PowerShell* is the recommended path for hand-written
  firmware development.  Zephyr's installer + Python venv work
  out of the box; the repo's `.vscode/tasks.json` uses
  platform-neutral `${command:cmake.buildDirectory}` paths so
  the build / debug experience is identical to macOS / Linux.
- *WSL2 (Ubuntu)* is the recommended path when you need full
  Linux toolchain behaviour for HIL automation, Yocto recipes,
  or exact CI parity.

Line endings are pinned to **LF** by `.gitattributes` so a
Windows checkout, a macOS clone, and a Linux pull see and commit
identical bytes -- avoids spurious clang-format-diff failures
when crossing hosts.

## Using with VS Code

The repo ships a `.vscode/` config that gets the standard Zephyr +
CMake + Cortex-Debug extensions working out of the box:

1. Open the repo in VS Code.  Accept the recommended-extensions
   prompt (`extensions.json` lists what to install).
2. Set `ZEPHYR_BASE` in your environment (one-time per workstation):
   ```powershell
   # Windows / PowerShell
   $env:ZEPHYR_BASE = "C:\path\to\zephyrproject\zephyr"
   ```
3. From the Command Palette → **Tasks: Run Task**, pick:
   - `validate · metadata` / `regen · soc_caps.h` / `regen · ABI snapshot` —
     no Zephyr workspace required, runs the SDK's Python tooling.
   - `twister · all (native_sim/native/64)` — runs the full ztest
     suite under Zephyr's host emulator.
   - `west build · edgeai-vision-aen` / `iot-connected-camera` —
     builds the example apps standalone.
4. C/C++ IntelliSense is wired through the CMake Tools extension.
   Run a `west build` task once and IntelliSense picks up the
   compile commands automatically.

The SDK consumes `<alp/...>` headers via the `include/` path; if
you're writing firmware against the SDK in a separate project,
add this repo as a Zephyr module (via `west.yml` or
`EXTRA_ZEPHYR_MODULES`) and your IntelliSense + build resolve the
headers transparently.

## Status

**v0.3 candidate** — recorded in
[`metadata/sdk_version.yaml`](metadata/sdk_version.yaml).  Surface
landed; runtime implementations fill in across point releases.  See
[`VERSIONS.md`](VERSIONS.md) for the version-by-version roadmap and
[`docs/os-support-matrix.md`](docs/os-support-matrix.md) for what's
GA / stub / planned per (library × OS × SoM).

### What's new in v0.3

- **`board.yaml` project config + loader** — single declarative file
  drives Zephyr / Yocto / bare-metal builds across every released
  MPN.  `scripts/alp_project.py` emits `alp.conf`, `alp.overlay`,
  CMake `-D` flags, Yocto `local.conf` snippets, and the
  `<alp_hw_info_build.h>` header from one input.  See
  [`docs/board-config.md`](docs/board-config.md).
- **SoM SKU presets shipped for every released MPN** — six AEN
  variants, two V2N, two V2M, one N93 placeholder.  Customer's
  board.yaml carries the MPN string; the SDK inherits silicon,
  on-module radio, secure element, RTC, EEPROM, default inference
  backend from the preset.
- **Hardware revision tracking** — per-family `hw-revisions.yaml`
  declares `[min_sdk_version, max_sdk_version]` windows; the
  loader + validator refuse to emit configs when the chosen
  `hw_rev` doesn't cover the SDK version.  Runtime check pairs an
  ADC + resistor-divider BOARD_ID pin with an EEPROM manifest
  carrying the exact MPN + serial + mfg date.
- **Inference backend dispatch expanded** — TFLite Micro driving
  Ethos-U (AEN + N93), DRP-AI3 (V2N), or DEEPX DX-M1 (V2M).
  Backend selected by `inference.backend:` in board.yaml; falls
  back to NOSUPPORT when the matching vendor SDK isn't installed.
- **20+ chip drivers** under `chips/<part>/` — LSM6DSO, BMI323,
  ICM-42670, BMP581, INA236, TMP112, RV-3028-C7, 24C128, CC3501E
  Wi-Fi/BLE coprocessor, TCAL9538, TAS2563 smart amp, OPTIGA Trust M,
  more.  Each opt-in via the EVK carrier preset's `populated:` list
  or the customer's override.
- **VS Code extension** (`vscode/`) — schema-aware `board.yaml`
  editor, GUI configurator with dropdowns / checkboxes driven by
  the live preset library, west wrappers, per-OS dependency
  bootstrap, inline validator diagnostics.

### What's new in v0.2

- **12 wrapped peripheral classes** (was 4): I2C, SPI, GPIO, UART,
  PWM, ADC, Counter + Quadrature decoder, I²S, CAN/CAN-FD, RTC,
  Watchdog.  See [ADR 0003](docs/adr/0003-peripheral-coverage.md).
- **`alp_last_error()`** thread-local diagnostic — apps that get NULL
  from `alp_*_open` can ask why.
- **SoC capability validation** — `<alp/soc_caps.h>` generated from
  `metadata/socs/**.json`.  See
  [ADR 0002](docs/adr/0002-error-mechanism.md).
- **`ALP_E1M_<CLASS>_COUNT`** portability macros.  See
  [ADR 0004](docs/adr/0004-e1m-portability-bound.md).
- **v0.2/v0.3 stub surfaces declared** for audio, BLE, security,
  multi-proc — real impl follows in target versions.
- **Architecture Decision Records** at [`docs/adr/`](docs/adr/).

## The stack

### AI framework (on-device)

- **TFLite Micro** dispatched to silicon-specific NPU back-ends:
  - **Arm Ethos-U** — Alif Ensemble (AEN family) + NXP i.MX 93 (N93 family; U55 / U65 variants)
  - **Renesas DRP-AI3** — RZ/V2N (V2N family)
  - **DEEPX DX-M1** — V2N + DX-M1 (V2M family); ONNX → DXNN compiler, model-family agnostic
  - **CPU** — reference-kernel fallback on any target
- Offline training (off-device) lives in TensorFlow / PyTorch.

### Dev tooling

- **`board.yaml` project config** — single source of truth: SoM SKU, carrier, OS, inference backend, libraries, peripherals, IoT toggles, diagnostics, `hw_rev`
- **`scripts/alp_project.py`** — emits Zephyr Kconfig fragments, plain-CMake `-D` flags, Yocto `local.conf` snippets, DTS overlays, or the `<alp_hw_info_build.h>` companion header
- **`scripts/validate_board_yaml.py`** — customer-side linter (exit 0 / 1 schema / 2 missing-preset / 3 hw_rev incompatible)
- **`tools/program_eeprom.py`** — packs board.yaml + serial + mfg date into the 128-byte EEPROM manifest for production-test programming
- **VS Code extension** (`vscode/`) — schema-aware `board.yaml` editor, GUI configurator, west wrappers, per-OS bootstrap, inline validator diagnostics

### Alp SDK (`<alp/...>`)

| Group | Headers + chip drivers |
|---|---|
| Peripherals | `peripheral.h` (GPIO/I²C/SPI/UART), `pwm.h`, `adc.h`, `counter.h`, `i2s.h`, `can.h`, `rtc.h`, `wdt.h`, `usb.h` |
| Audio / camera / display | `audio.h` (PDM in + I²S out), `camera.h`, `gui.h` (LVGL) — chip drivers: SSD1306, SSD1331, OV5640, CAM_MUX, TAS2563, PDM mic |
| Connectivity & security | `iot.h` (Wi-Fi/MQTT), `ble.h` (BLE 5.4), `security.h` (MbedTLS PSA Crypto), OPTIGA Trust M chip driver |
| Inference dispatcher | `inference.h` — backend selector (auto / cpu / ethos_u / drpai / deepx_dx) + tensor-arena management |
| Multi-proc / IPC | `mproc.h` — mailbox + shared memory + hardware semaphore |
| Hardware info | `hw_info.h` — 128-byte EEPROM manifest + BOARD_ID ADC + `assert_matches_build()` |
| Chip drivers | 20+ under `chips/` — LSM6DSO, BMI323, ICM-42670, BMP581, INA236, TMP112, RV-3028-C7, 24C128, CC3501E, TCAL9538, button-LED helper, … |
| User libraries (via `libraries:` in board.yaml) | ETL · fmt · nlohmann_json · doctest · LVGL · MbedTLS · CMSIS-DSP · LittleFS |

### OS backend

Bare-metal · Yocto · Zephyr.  Selected by `os:` in `board.yaml`.

### Vendor SDK

Alif Ensemble · Renesas RZ/V2N · NXP i.MX 93 · DEEPX DX-M1 · TI SimpleLink (CC3501E coprocessor).

### HW + HAL

E1M (35×35 mm) and E1M-X (45×65 mm) SoMs · E1M-EVK and E1M-X-EVK reference carriers · vendor HALs.

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│  E1M™ — Scalable AI Smarter Edge                              ⚡ Alp Lab     │
└──────────────────────────────────────────────────────────────────────────────┘

  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │ AI Framework  │ ─► │  TFLM   →  Ethos-U   (Alif AEN, NXP N93 / U55+U65) │
  │  (on-device)  │    │         →  DRP-AI3   (Renesas V2N)                 │
  │               │    │         →  DEEPX DX-M1  (V2M family)               │
  │               │    │         →  CPU       (reference kernels)           │
  │               │    │  · · · offline training only: TensorFlow, PyTorch  │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │ Dev Tooling   │ ─► │  VS Code extension  ·  board.yaml + loader         │
  │   (v0.3 NEW)  │    │  validate_board_yaml.py  ·  GUI configurator       │
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
  │               │    │  Display / GUI        HW Info         Multi-proc   │
  │               │    │  ─ SSD1306 / 1331     ─ EEPROM mfst   ─ Mailbox    │
  │               │    │  ─ LVGL               ─ BOARD_ID ADC  ─ Shared mem │
  │               │    │                       ─ <alp/hw_info> ─ HW sem     │
  │               │    │                                                    │
  │               │    │  ─── 20+ chip drivers (lsm6dso, bmi323, bmp581,    │
  │               │    │       icm42670, ina236, tmp112, tcal9538,          │
  │               │    │       rv3028c7, 24c128, cc3501e, ssd13xx, ...)     │
  │               │    │                                                    │
  │               │    │  ─── User libraries (board.yaml `libraries:`):     │
  │               │    │       ETL · fmt · nlohmann_json · doctest          │
  │               │    │       LVGL · MbedTLS · CMSIS-DSP · LittleFS        │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │      OS       │ ─► │   Bare-metal         Yocto            Zephyr       │
  └───────────────┘    └────────────────────────────────────────────────────┘
          │
  ┌───────────────┐    ┌────────────────────────────────────────────────────┐
  │  Vendor SDK   │ ─► │   Alif Ensemble (AEN)   Renesas RZ/V2N             │
  │               │    │   NXP i.MX 93           DEEPX DX-M1                │
  │               │    │   TI SimpleLink (CC3501E coprocessor)              │
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

| Header               | Library                                    | Status                                |
|----------------------|--------------------------------------------|---------------------------------------|
| `alp/peripheral.h`   | I2C, SPI, GPIO, UART                       | GA on Zephyr (AEN + native_sim)       |
| `alp/pwm.h`          | PWM                                        | GA on Zephyr                          |
| `alp/adc.h`          | ADC                                        | GA on Zephyr                          |
| `alp/counter.h`      | Counter + Quadrature decoder               | GA on Zephyr                          |
| `alp/i2s.h`          | I²S / SAI                                  | GA on Zephyr                          |
| `alp/can.h`          | CAN / CAN-FD                               | GA on Zephyr                          |
| `alp/rtc.h`          | RTC                                        | GA on Zephyr                          |
| `alp/wdt.h`          | Watchdog                                   | GA on Zephyr                          |
| `alp/usb.h`          | USB device                                 | Surface declared (v0.3); host role v0.4 |
| `alp/camera.h`       | Camera                                     | Surface declared, impl rolling per-SoM |
| `alp/gui.h`          | GUI / LVGL                                 | LVGL re-export                        |
| `alp/iot.h`          | Wi-Fi station + MQTT                       | Real Zephyr impl wired (v0.3)         |
| `alp/audio.h`        | Audio (PDM in / I²S out)                   | Real impl wired (v0.3)                |
| `alp/ble.h`          | BLE peripheral + central                   | Real impl wired (v0.3)                |
| `alp/security.h`     | MbedTLS PSA Crypto (hash / AEAD / TRNG)    | Real impl wired (v0.3)                |
| `alp/mproc.h`        | Multi-proc IPC (mailbox / shared mem / hwsem) | Real impl wired (v0.3)             |
| `alp/inference.h`    | Inference dispatcher (TFLM / Ethos-U / DRP-AI / DEEPX) | Real impl wired (v0.3); per-vendor link arrives with the vendor SDK |
| `alp/hw_info.h`      | Hardware identification (EEPROM manifest + BOARD_ID ADC) | Surface declared (v0.3); runtime read lands v0.3.x |
| `alp/soc_caps.h`     | (generated) Active-SoC capability constants | Generated from `metadata/socs/**.json` |
| `alp/e1m_pinout.h`   | E1M-spec instance IDs + portability bounds | Pinned to e1m-spec v1.0               |
| `alp/boards/<carrier>.h` | Carrier-feature names (e.g. EVK pin map) | Pinned to EVK + X-EVK reference designs |
| `chips/<part>/`      | 20+ chip drivers, opt-in via `board.yaml` `carrier.populated:` | Driver-side complete; runtime exercised per (chip × OS) combo |

## Supported hardware

The SDK targets three SoM **families**.  Within each family every SKU
shares the same E1M routing and the same vendor HAL — they differ
only in SoC variant and memory tier — so a single backend covers the
whole family.

| Family             | Form factor       | SKUs                                                                                  | Primary silicon                                              | AI throughput | OS targets         |
|--------------------|-------------------|---------------------------------------------------------------------------------------|--------------------------------------------------------------|---------------|--------------------|
| **E1M-AEN**        | E1M (35×35 mm)    | `E1M-AEN301`, `E1M-AEN401`, `E1M-AEN501`, `E1M-AEN601`, `E1M-AEN701`, `E1M-AEN801`    | Alif Semiconductor *Ensemble* E3–E8 (Cortex-M55 + optional Cortex-A32 + Ethos-U55) | up to ~1024 GOPS | Zephyr · bare-metal |
| **E1M-X V2N**      | E1M-X (45×65 mm)  | `E1M-V2N101`, `E1M-V2N102`                                                            | Renesas RZ/V2N (4× Cortex-A55 + Cortex-M33 + DRP-AI3)        | 4 TOPS        | Yocto              |
| **E1M-X V2N-M1**   | E1M-X (45×65 mm)  | `E1M-V2M101`, `E1M-V2M102`                                                            | Renesas RZ/V2N + DeepX DX-M1                                 | 4 + 25 TOPS   | Yocto              |
| **E1M-i.MX93**     | E1M (35×35 mm)    | TBD                                                                                   | NXP i.MX 93 (2× Cortex-A55 + Cortex-M33 + Ethos-U65)         | ~0.5 TOPS     | Yocto              |

All modules use the **E1M open-standard form factor**.  HW pinout and
mechanical specification live in
[`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec) — pinned
to **v1.1** for this revision of the SDK.

### Evaluation kits

ALP Lab ships two carrier boards:

- **E1M EVK** (UG-E1M-001) — for the 35 × 35 mm E1M form factor.
  Hosts the E1M-AEN family and the E1M-i.MX93 family.  USB, dual
  Ethernet, CAN, MIPI DSI, three camera options, audio, on-board
  sensors (ICM-42670-P, BMI323, BMP581), M.2 (Key M + Key E),
  Arduino + mikroBUS.  See [`docs/boards/e1m-evk.md`](docs/boards/e1m-evk.md).
- **E1M-X EVK** — for the 45 × 65 mm E1M-X form factor.  Hosts the
  E1M-X V2N and V2N-M1 families.  Two CSI camera connectors, two
  DSI display outputs, dual GbE, expanded M.2 sizing for the
  V2N+M1 PCIe layout, mikroBUS expansion.  See
  [`docs/boards/e1m-x-evk.md`](docs/boards/e1m-x-evk.md).

### Platform invariants

The SDK relies on the following hardware invariants that
[`e1m-spec` §6.5](https://github.com/alplabai/e1m-spec/blob/main/STANDARD.md#65-mandatory-on-module-components)
makes normative for every conformant E1M / E1M-X SoM:

- **On-module Ethernet PHY(s).**  At least one PHY is on the module;
  E1M-X SoMs with two MAC controllers populate two.  E1M-X `ETH*_*`
  pads are therefore post-PHY differential MDI — the SDK's IoT
  library targets these pads directly without configuring an external
  PHY.
- **On-module Wi-Fi 6 + BLE 5.4 combo.**  2.4 GHz and 5 GHz are
  always available; 6 GHz is optional per SoM.  The `alp/iot.h`
  Wi-Fi-station + MQTT path assumes the combo radio is present.
- **On-module CAN transceiver(s).**  When any CAN group is exposed,
  the carrier-side pads are bus-level (`CANxH` / `CANxL`).  The SDK's
  CAN APIs therefore drive a bus, not a TX/RX controller pair.
- **Supervisory MCU on V2N family.**  The `E1M-X V2N` and
  `E1M-X V2N-M1` SoMs include a separate **GigaDevice GD32G553**
  Cortex-M33 supervisor (216 MHz) alongside the Renesas internal
  Cortex-M33 (200 MHz).  See
  [`vendors/renesas-rzv2n/README.md`](vendors/renesas-rzv2n/README.md)
  for what the supervisor handles vs. the application core.

## Consuming the SDK

The SDK can be consumed two ways:

### As a Zephyr module (recommended for Zephyr projects)

Add to your application's `west.yml`:

```yaml
manifest:
  projects:
    - name: alp-sdk
      url: https://github.com/alplabai/alp-sdk
      revision: v0.1.0
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

# Zephyr (E1M-AEN, after the v0.1 implementation lands)
west init -m https://github.com/alplabai/alp-sdk --mr v0.1.0 alp-ws
cd alp-ws && west update
west build -b alif_e7_dk_rtss_he tests/zephyr/peripheral
```

## Repository layout

```
alp-sdk/
├── include/alp/             # PUBLIC headers (the consumer surface)
├── src/
│   ├── common/              # OS-agnostic helpers
│   ├── zephyr/              # Zephyr backend
│   ├── baremetal/           # bare-metal backend
│   └── yocto/               # Linux/userspace backend
├── chips/                   # 20+ opt-in chip drivers (lsm6dso, ssd1306, ...)
├── vendors/
│   ├── alif/                # Alif Ensemble HAL bindings
│   ├── renesas-rzv2n/       # RZ/V2N HAL bindings
│   ├── nxp-imx93/           # NXP i.MX 93 HAL bindings
│   └── deepx-dxm1/          # DEEPX DX-M1 host-side bindings
├── metadata/
│   ├── sdk_version.yaml     # SDK release version (declared)
│   ├── schemas/             # JSON Schemas (board-config-v1, soc-spec-v1)
│   ├── templates/           # canonical board.yaml template + example
│   ├── e1m_modules/<MPN>/   # one som.yaml per released MPN
│   ├── e1m_modules/<family>/hw-revisions.yaml  # ADC + SDK-version compat
│   ├── carriers/<name>/     # EVK + X-EVK + custom carrier presets
│   ├── library-profiles/    # compile-time profile headers per library
│   └── socs/                # per-SoC capability JSONs (drives soc_caps.h)
├── scripts/
│   ├── alp_project.py       # board.yaml loader (5 emit modes)
│   ├── validate_board_yaml.py  # customer-side linter (exit 0/1/2/3)
│   ├── validate_metadata.py # CI schema gate for metadata/socs
│   ├── gen_soc_caps.py      # generates include/alp/soc_caps.h
│   └── abi_snapshot.py      # docs/abi/v*-snapshot.json generator
├── tools/
│   └── program_eeprom.py    # production-test EEPROM manifest writer
├── vscode/                  # in-tree VS Code extension (TypeScript)
├── examples/                # 13 reference apps (gpio-button-led, i2c-scanner, ...)
├── docs/
│   ├── architecture.md
│   ├── board-config.md      # board.yaml schema reference
│   ├── cc3501e-bridge.md    # CC3501E Wi-Fi/BLE coprocessor bridge
│   ├── os-support-matrix.md
│   ├── porting-new-som.md
│   └── adr/                 # architecture decision records
├── tests/
│   ├── scripts/             # Python unit tests for the loader + validators
│   └── zephyr/              # ztest suites under native_sim
├── firmware/cc3501e/        # CC3501E prebuilt firmware blobs
├── west.yml                 # Zephyr manifest
├── zephyr/module.yml        # Zephyr-module discovery
└── CMakeLists.txt           # plain-CMake entry point
```

See [`docs/porting-new-som.md`](docs/porting-new-som.md) for adding a
new E1M variant and [`docs/board-config.md`](docs/board-config.md) for
the `board.yaml` schema reference.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

Copyright 2026 ALP Lab AB.
