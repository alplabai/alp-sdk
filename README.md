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

v0.1.0 → v0.2.0 — **scaffolding plus expanded peripheral coverage**.
Public-header surface is in place; the v0.2 work doubled wrapped
peripheral classes from 4 to 12 and added the diagnostic /
capability-validation infrastructure that future implementations rely
on.  See [`docs/os-support-matrix.md`](docs/os-support-matrix.md) for
what's GA, stub, or planned per (library × OS × SoM).

### What's new in v0.2

- **12 wrapped peripheral classes** (was 4): I2C, SPI, GPIO, UART,
  PWM, ADC, Counter + Quadrature decoder, I²S, CAN/CAN-FD, RTC,
  Watchdog.  See [ADR 0003](docs/adr/0003-peripheral-coverage.md).
- **`alp_last_error()`** thread-local diagnostic — apps that get NULL
  from `alp_*_open` can ask why (`ALP_ERR_INVAL` /
  `ALP_ERR_OUT_OF_RANGE` / `ALP_ERR_NOT_READY` / `ALP_ERR_NOMEM` / …).
- **SoC capability validation** — `<alp/soc_caps.h>` is generated
  from `metadata/socs/**.json` and rejects configs that exceed the
  active SoC's documented hardware caps.  Canonical case: a 16-bit
  ADC request on a 12-bit SoC fails at `alp_adc_open` with
  `ALP_ERR_OUT_OF_RANGE`, before any I/O.  See
  [ADR 0002](docs/adr/0002-error-mechanism.md).
- **`ALP_E1M_<CLASS>_COUNT`** macros document the cross-SoM-portable
  instance count per class — apps that stay below the bound work on
  every E1M-conformant SoM.  See
  [ADR 0004](docs/adr/0004-e1m-portability-bound.md).
- **v0.2/v0.3 surfaces declared** — `<alp/audio.h>`, `<alp/ble.h>`,
  `<alp/security.h>`, `<alp/mproc.h>` ship as compile-clean stubs
  returning `ALP_ERR_NOSUPPORT`; real impls land in their target
  versions.
- **Architecture Decision Records** at [`docs/adr/`](docs/adr/) —
  four records covering the wrapper-vs-Zephyr boundary, the error
  mechanism, peripheral coverage, and the E1M portability bound.

## The stack

```
Application code
       │
       ▼
+---------------------------------------------------------+
|  ALP SDK  ─  <alp/peripheral.h>, <alp/display.h>, ...   |  ← this repo
+---------------------------------------------------------+
       │
       ▼
  Zephyr / Yocto / Bare-metal  →  Vendor HAL  →  CMSIS  →  Silicon
```

See [`docs/architecture.md`](docs/architecture.md) for the full layered
diagram and per-library design.

## Public API

All consumer-facing headers live under `include/alp/`:

| Header               | Library         | Status                                |
|----------------------|-----------------|---------------------------------------|
| `alp/peripheral.h`   | I2C, SPI, GPIO, UART | v0.1 GA on Zephyr-AEN            |
| `alp/pwm.h`          | PWM             | **v0.2** GA on Zephyr-AEN             |
| `alp/adc.h`          | ADC             | **v0.2** GA on Zephyr-AEN             |
| `alp/counter.h`      | Counter + Quadrature decoder | **v0.2** GA on Zephyr-AEN |
| `alp/i2s.h`          | I²S / SAI       | **v0.2** GA on Zephyr-AEN             |
| `alp/can.h`          | CAN / CAN-FD    | **v0.2** GA on Zephyr-AEN             |
| `alp/rtc.h`          | RTC             | **v0.2** GA on Zephyr-AEN             |
| `alp/wdt.h`          | Watchdog        | **v0.2** GA on Zephyr-AEN             |
| `alp/display.h`      | Display         | Surface declared (SSD1306 first)      |
| `alp/camera.h`       | Camera          | Stub — v0.2 ships MIPI CSI-2          |
| `alp/gui.h`          | GUI / LVGL      | LVGL re-export                        |
| `alp/math.h`         | Math            | CMSIS-DSP re-export                   |
| `alp/signal.h`       | Signal          | Forward marker — v0.2 audio           |
| `alp/iot.h`          | IoT             | Stub; real Wi-Fi+MQTT in v0.2         |
| `alp/audio.h`        | Audio (PDM in / I²S out) | Surface declared, impl v0.2  |
| `alp/ble.h`          | BLE peripheral + central | Surface declared, impl v0.3  |
| `alp/security.h`     | Hash / AEAD / TRNG       | Surface declared, impl v0.3  |
| `alp/mproc.h`        | Multi-proc IPC  | Surface declared, impl v0.3           |
| `alp/soc_caps.h`     | (generated) Active-SoC capability constants | Generated from metadata |
| `alp/e1m_pinout.h`   | E1M-spec instance IDs + portability bounds | Pinned to e1m-spec v1.0 |

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
├── include/alp/         # PUBLIC headers (the consumer surface)
├── src/
│   ├── common/          # OS-agnostic helpers
│   ├── zephyr/          # Zephyr backend
│   ├── baremetal/       # bare-metal backend
│   └── yocto/           # Linux/userspace backend
├── vendors/
│   ├── alif/            # Alif Ensemble HAL bindings
│   └── renesas-rzv2n/   # RZ/V2N HAL bindings (v0.2)
├── cmake/               # find_package + Zephyr module helpers
├── docs/
│   ├── architecture.md
│   ├── os-support-matrix.md
│   └── porting-new-som.md
├── tests/
├── west.yml             # Zephyr manifest
├── zephyr/module.yml    # Zephyr-module discovery
└── CMakeLists.txt       # plain-CMake entry point
```

See [`docs/porting-new-som.md`](docs/porting-new-som.md) for adding a
new E1M variant.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

Copyright 2026 ALP Lab AB.
