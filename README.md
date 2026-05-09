# ALP SDK — Unification SDK for E1M Edge AI Modules

> Write once, run on any E1M module.

The **ALP SDK** is the unification software layer for ALP Lab edge AI
modules built on the **E1M open-standard form factor**.  It provides
application developers a single C/C++ API that works across every
E1M-* SoM variant — present and future — by wrapping each vendor's
SDK on top of ARM CMSIS.

Supported OS targets: **Bare-metal · Zephyr RTOS · Yocto Linux**.
FreeRTOS is intentionally not a target.

## Status

v0.1.0 — **scaffolding**.  Public-header surface is in place; backend
implementations land per peripheral after sign-off.  See
[`docs/os-support-matrix.md`](docs/os-support-matrix.md) for what's GA,
stub, or planned per (library × OS × SoM).

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

| Header               | Library         | v0.1 status                 |
|----------------------|-----------------|-----------------------------|
| `alp/peripheral.h`   | I2C, SPI, GPIO, UART | Surface declared        |
| `alp/display.h`      | Display         | Surface declared (SSD1306 first) |
| `alp/camera.h`       | Camera          | Stub — v0.2 ships MIPI CSI-2  |
| `alp/gui.h`          | GUI / LVGL      | LVGL re-export                |
| `alp/math.h`         | Math            | CMSIS-DSP re-export           |
| `alp/signal.h`       | Signal          | Forward marker — v0.2 audio   |
| `alp/iot.h`          | IoT             | Wi-Fi-station + MQTT          |

## Supported hardware

| Module       | Processor                  | AI throughput | OS targets        |
|--------------|----------------------------|---------------|-------------------|
| E1M-AEN      | Alif Ensemble (Cortex-M55 + Ethos-U55) | 1024 GOPS | Zephyr · bare-metal |
| E1M-X-V2N    | Renesas RZ/V2N             | 4 TOPS        | Yocto             |
| E1M-X-V2N-M1 | RZ/V2N + DeepX M1          | 25 TOPS       | Yocto             |

All modules use the **E1M open-standard form factor**.  HW pinout
lives in [`alpCaner/e1m-spec`](https://github.com/alpCaner/e1m-spec).

## Consuming the SDK

The SDK can be consumed two ways:

### As a Zephyr module (recommended for Zephyr projects)

Add to your application's `west.yml`:

```yaml
manifest:
  projects:
    - name: alp-sdk
      url: https://github.com/alpCaner/alp-sdk
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
west init -m https://github.com/alpCaner/alp-sdk --mr v0.1.0 alp-ws
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
