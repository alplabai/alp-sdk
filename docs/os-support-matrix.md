# OS Support Matrix

Status keys:
- **GA** — implemented, tested on real silicon, in CI.
- **stub** — header surface compiles; functions return `ALP_ERR_NOSUPPORT`.
- **planned** — declared roadmap, no code yet.
- **n/a** — combination not targeted.

Each column targets a SoM **family** rather than a single SKU.  Within
a family every SKU shares the same E1M routing and the same vendor
HAL, so a single backend covers the whole family.  See
[`README.md` § Supported hardware](../README.md#supported-hardware)
for the SKU breakdown:

- **E1M-AEN family** — `E1M-AEN301` … `E1M-AEN801` (Alif Ensemble
  E3–E8).
- **E1M-X V2N family** — `E1M-V2N101`, `E1M-V2N102` (Renesas RZ/V2N).
- **E1M-X V2N-M1 family** — `E1M-V2M101`, `E1M-V2M102` (RZ/V2N +
  DeepX DX-M1).

## v0.1.0

| Library     | E1M-AEN / Bare-metal | E1M-AEN / Zephyr | E1M-X V2N / Yocto | E1M-X V2N-M1 / Yocto |
|-------------|----------------------|------------------|-------------------|----------------------|
| Peripherals (I2C/SPI/GPIO/UART) | stub | **GA**         | stub              | stub                 |
| Display     | n/a                  | **GA** (SSD1306) | stub              | stub                 |
| Camera      | n/a                  | stub             | stub (planned v0.2 MIPI CSI-2) | stub (planned v0.2) |
| GUI/LVGL    | n/a                  | **GA** (re-export) | planned         | planned              |
| Math        | **GA** (re-export)   | **GA** (re-export) | **GA**          | **GA**               |
| Signal      | stub                 | stub             | stub              | stub                 |
| IoT         | n/a                  | stub (real Wi-Fi+MQTT in v0.2) | stub | stub                |

## v0.2.0 — landed (peripheral expansion + capability validation)

The v0.2 SDK doubles peripheral coverage from 4 to 12 wrapped
classes plus the diagnostic / validation infrastructure that
underpins them.  Surface-only deliverables ship the public header
+ a stub backend; full implementations land per the per-class
plan in `VERSIONS.md`.

| Library                   | E1M-AEN / Bare-metal | E1M-AEN / Zephyr | E1M-X V2N / Yocto | E1M-X V2N-M1 / Yocto |
|---------------------------|----------------------|------------------|-------------------|----------------------|
| **PWM** (`<alp/pwm.h>`)   | planned              | **GA** (Zephyr `pwm_*`) | planned    | planned              |
| **ADC** (`<alp/adc.h>`)   | planned              | **GA** (Zephyr `adc_*`) | planned    | planned              |
| **Counter / QEnc** (`<alp/counter.h>`) | planned | **GA** (Zephyr `counter_*` + `sensor_*`) | planned | planned |
| **I²S / SAI** (`<alp/i2s.h>`) | planned          | **GA** (Zephyr `i2s_*`) | planned    | planned              |
| **CAN / CAN-FD** (`<alp/can.h>`) | planned       | **GA** (Zephyr `can_*`) | planned    | planned              |
| **RTC** (`<alp/rtc.h>`)   | planned              | **GA** (Zephyr `rtc_*`) | planned    | planned              |
| **Watchdog** (`<alp/wdt.h>`) | planned           | **GA** (Zephyr `wdt_*`) | planned    | planned              |
| **Audio** (`<alp/audio.h>`) | n/a                | surface declared (impl v0.2) | planned | planned         |
| **Camera** (`<alp/camera.h>`) | n/a              | planned          | **GA** (MIPI CSI-2) | **GA**             |
| **IoT** (`<alp/iot.h>`)   | n/a                  | **GA** (Wi-Fi+MQTT) | **GA**         | **GA**               |

### Cross-cutting v0.2 capability infrastructure

| Capability                                      | Status | Notes |
|-------------------------------------------------|--------|-------|
| `alp_last_error()` thread-local                 | **GA** (Zephyr) | Stamped by every `*_open` failure path.  See ADR 0002. |
| `<alp/soc_caps.h>` generated capability tables  | **GA** | From `metadata/socs/**.json` via `gen_soc_caps.py`.  Selected by `CONFIG_ALP_SOC_<TOKEN>`. |
| `ALP_E1M_<CLASS>_COUNT` portability bounds      | **GA** | Cross-SoM-portable instance count per class.  See ADR 0004. |
| ABI snapshot diff tool                          | **GA** | `scripts/abi_snapshot.py --diff prior.json`. |
| `pr-generated-files.yml` CI workflow            | **GA** | Catches stale `soc_caps.h` / ABI snapshot. |

## v0.3.0 — declared (surfaces ship in v0.1, impl arrives in v0.3)

| Library              | Surface | Backed-by plan |
|----------------------|---------|----------------|
| BLE (`<alp/ble.h>`)  | declared| Zephyr `bt` host stack |
| Security (`<alp/security.h>`) | declared | MbedTLS PSA + per-SoC HW accelerator routing |
| MProc (`<alp/mproc.h>`) | declared | Zephyr `mbox_*` (MHU on Alif) + `hwsem_*` + shared-memory regions |

## v0.4.0 prep — landed on `main` (2026-05-11)

Yocto-side surface advances ahead of the v0.4 tag.  Each item is
gated by either a CMake `find_package` / `pkg_check_modules` check
or a default-disabled `west.yml` group, so workspaces that don't
need v0.4 fall back cleanly to the v0.3 state above.

| Library                              | E1M-AEN / Bare-metal | E1M-AEN / Zephyr | E1M-X V2N / Yocto | E1M-X V2N-M1 / Yocto |
|--------------------------------------|----------------------|------------------|-------------------|----------------------|
| **Peripherals (I2C)** (`<alp/peripheral.h>`) | planned   | **GA**           | **GA** (i2c-dev) | **GA** (i2c-dev)     |
| **Peripherals (SPI)** (`<alp/peripheral.h>`) | planned   | **GA**           | **GA** (spidev)  | **GA** (spidev)      |
| **Peripherals (UART)** (`<alp/peripheral.h>`)| planned   | **GA**           | **GA** (termios) | **GA** (termios)     |
| **Peripherals (GPIO + IRQ)** (`<alp/peripheral.h>`) | planned | **GA**    | **GA** (chardev v2 + pthread `poll()`) | **GA** |
| **IoT — MQTT** (`<alp/iot.h>`)       | n/a                  | planned (Zephyr `mqtt_*`) | **GA** (libmosquitto) | **GA** (libmosquitto) |
| **IoT — Wi-Fi station** (`<alp/iot.h>`) | n/a               | planned          | stub (system-config via wpa_supplicant/NM) | stub |

The Yocto MQTT build is conditional on `pkg_check_modules(libmosquitto)`;
workspaces without libmosquitto-dev on the sysroot keep the
NOSUPPORT stubs.  Per-class `ALP_VENDOR_OVERRIDES_<CLASS>` macros
in `src/common/stub_backend.c` let each peripheral class roll out
independently across backends.

## CMSIS-DSP per-SoM validation

ALP SDK does not re-export CMSIS-DSP -- application code includes
`arm_math.h` directly when DSP/math primitives are needed.  ALP's
own internals (e.g. filtering inside `<alp/audio.h>` ) optionally
pull in CMSIS-DSP when the build sets `ALP_HAS_CMSIS_DSP`.  Either
way the validated **feature groups** depend on each SoC's
instruction set extensions:

| Family         | Core(s)                  | DSP-ext | Helium (MVE) | Validated CMSIS-DSP groups (v0.1)               |
|----------------|--------------------------|---------|--------------|-------------------------------------------------|
| E1M-AEN        | Cortex-M55 + Ethos-U55   | yes     | yes          | Filtering, Transform, Matrix, Statistics, NN.   |
| E1M-X V2N      | Cortex-A55 + DRP-AI3     | n/a (A) | n/a (NEON)   | All scalar paths; NEON used where available.    |
| E1M-X V2N-M1   | Cortex-A55 + DRP-AI3 + M1| n/a (A) | n/a (NEON)   | Same as V2N (M1 is an external NPU).            |

Helium-accelerated paths are only enabled when the build sets
`-DALP_CMSIS_DSP_HELIUM=ON` and the toolchain confirms `__ARM_FEATURE_MVE`.
