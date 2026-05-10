# ALP SDK Architecture

The ALP SDK is the **unification software layer** for ALP Lab edge AI
modules built on the **E1M open-standard form factor**. It provides
application developers a single C/C++ API that works across every
E1M-* SoM variant — present and future — by wrapping each vendor's
SDK on top of ARM CMSIS.

## The stack

```
┌─────────────────────────────────────────────────────────────┐
│  Application / Edge-AI App / IoT App                        │  ← user code
├─────────────────────────────────────────────────────────────┤
│  Zephyr RTOS  /  Yocto Linux  /  Bare Metal                 │  ← OS, picked per variant
├─────────────────────────────────────────────────────────────┤
│  ALP SDK                                                    │  ← THIS REPO
│    Libraries:  GUI/LVGL · Display · Camera · Math ·         │
│                Signal Processing · IoT · Peripherals        │
│    Vendor wrappers:  Alif HAL · Renesas RZ · ...            │
│    Chip drivers:  lsm6dso · ssd1306 · ov5640 · bme280 · ... │
│    Chip metadata: metadata/socs/<vendor>/<family>/<part>    │
├─────────────────────────────────────────────────────────────┤
│  ARM CMSIS                                                  │
├─────────────────────────────────────────────────────────────┤
│  Vendor HAL                                                 │
├─────────────────────────────────────────────────────────────┤
│  SoM Hardware (E1M-AEN, E1M-X-V2N, ...)                     │
└─────────────────────────────────────────────────────────────┘
```

The SDK pivots on **OS** above (which backend to compile), and on
**vendor SoC** below (which HAL/CMSIS pack to call into).  Applications
talk only to `<alp/...>` headers; everything below is replaceable.

## OS targets

The SDK explicitly supports three operating-system targets — and only
these three.

| OS         | When used                                                                  | Backend dir          |
|------------|----------------------------------------------------------------------------|----------------------|
| Bare metal | Tightest-budget M-class workloads (E1M-AEN without networking).            | `src/baremetal/`     |
| Zephyr     | Default RTOS for RTOS-class workloads.  Mainline Zephyr + Alif HAL.        | `src/zephyr/`        |
| Yocto Linux| Linux-class variants (E1M-X-V2N family).                                   | `src/yocto/`         |

Choosing the OS for a given SoM is a board/integrator decision; the
SDK build picks the matching backend from `src/` based on whether
`ZEPHYR_BASE` is defined in the consuming build, on `CONFIG_ALP_OS`
(plain-CMake), or on detection of a Yocto SDK toolchain.

## Repository layout

```
alp-sdk/
├── README.md
├── LICENSE                          # Apache-2.0
├── docs/
│   ├── architecture.md              # this file
│   ├── os-support-matrix.md         # OS × variant × library status
│   └── porting-new-som.md           # adding HAL/HW for a new E1M variant
├── include/alp/                     # PUBLIC HEADERS — the consumer surface
│   ├── peripheral.h                 # alp_i2c_t, alp_spi_t, alp_gpio_t, alp_uart_t
│   ├── display.h
│   ├── camera.h
│   ├── gui.h                        # LVGL re-export with ALP defaults
│   ├── math.h                       # CMSIS-DSP re-export
│   ├── signal.h
│   └── iot.h
├── src/
│   ├── common/                      # OS-agnostic helpers (bit ops, ring buffers, status->str)
│   ├── zephyr/                      # Zephyr-backed implementation
│   ├── baremetal/                   # bare-metal implementation
│   └── yocto/                       # Linux/userspace implementation
├── chips/                           # CHIP DRIVER IMPLEMENTATIONS — one dir per IC
│   ├── lsm6dso/                     # symbols are lsm6dso_* (no alp_ prefix on chip drivers)
│   ├── ssd1306/
│   └── ...
├── vendors/
│   ├── alif/                        # Alif HAL bindings (start here for v0.1)
│   └── renesas-rzv2n/               # stub for v0.2
├── metadata/                        # CHIP METADATA — alp-studio's soc_ref resolves here
│   ├── schemas/soc-spec-v1.schema.json
│   └── socs/alif/ensemble/{e3,e4,e5,e6,e7,e8}.json
├── cmake/                           # find_package + Zephyr module helpers
│   └── AlpSdkConfig.cmake.in
├── west.yml                         # Zephyr-side manifest
├── zephyr/
│   ├── module.yml                   # makes the repo importable as a Zephyr module
│   └── Kconfig                      # ALP_SDK_* options exposed to Zephyr apps
├── ci/                              # GitHub Actions workflows (mirrored into .github/workflows/)
├── yocto/meta-alp/                  # Yocto BSP layer (v0.4+; placeholder before then)
└── tests/                           # Unity / ztest smoke tests, QEMU + real silicon
```

The public surface is **only** `include/alp/`.  Anything under `src/`,
`vendors/`, or `cmake/` is implementation; consumers must not include
those headers directly.

## Library design

### Peripheral primitives

| Class            | Header              | Backed by Zephyr            | v0.1 | v0.2 |
|------------------|---------------------|-----------------------------|------|------|
| I2C              | `alp/peripheral.h`  | `i2c_*`                     | ✓    | ✓    |
| SPI              | `alp/peripheral.h`  | `spi_*`                     | ✓    | ✓    |
| GPIO             | `alp/peripheral.h`  | `gpio_*`                    | ✓    | ✓    |
| UART             | `alp/peripheral.h`  | `uart_*`                    | ✓    | ✓    |
| PWM              | `alp/pwm.h`         | `pwm_*`                     |      | ✓    |
| ADC              | `alp/adc.h`         | `adc_*` + `adc_dt_spec`     |      | ✓    |
| Counter / Timer  | `alp/counter.h`     | `counter_*`                 |      | ✓    |
| Quadrature decoder | `alp/counter.h`   | `sensor_*` (SENSOR_CHAN_ROTATION) |    | ✓    |
| I2S / SAI        | `alp/i2s.h`         | `i2s_*` + memory slab       |      | ✓    |
| CAN / CAN-FD     | `alp/can.h`         | `can_*` (FD via `CAN_MODE_FD`) |   | ✓    |
| RTC              | `alp/rtc.h`         | `rtc_*`                     |      | ✓    |
| Watchdog         | `alp/wdt.h`         | `wdt_*` + `wdt_install_timeout` |  | ✓    |

See [ADR 0003](adr/0003-peripheral-coverage.md) for why this list and
not others.

### Higher libraries

| Library          | Header(s)            | Backed by                                                      | Status |
|------------------|----------------------|----------------------------------------------------------------|--------|
| Display          | `alp/display.h`      | Zephyr `display_*` (SSD1306 first).                            | v0.1 surface; full impl v0.3 |
| Camera           | `alp/camera.h`       | Zephyr `video_*` API.  V2N MIPI CSI-2 wrapper in v0.2.          | v0.1 stub (NOSUPPORT) |
| GUI/LVGL         | `alp/gui.h`          | Upstream LVGL with an ALP `lv_conf.h`.                         | Header re-export only — no custom widgets |
| Math             | `alp/math.h`         | CMSIS-DSP (`arm_math.h`).                                      | Re-export.  Per-SoM feature validation in `os-support-matrix.md` |
| Signal Proc.     | `alp/signal.h`       | Re-exports CMSIS-DSP filters via `<alp/math.h>`.               | Forward marker; audio helpers in v0.2 |
| IoT              | `alp/iot.h`          | Zephyr `net_*` + MQTT client (AEN); Linux net + Mosquitto (Yocto). | v0.1 stub; real Wi-Fi+MQTT v0.2 |
| Audio            | `alp/audio.h`        | Zephyr `audio_dmic` + `i2s_*` chains; ALSA on Yocto.            | v0.1 surface; impl v0.2 |
| BLE              | `alp/ble.h`          | Zephyr `bt` host stack (peripheral + central + GATT).           | v0.1 surface; impl v0.3 |
| Security         | `alp/security.h`     | MbedTLS PSA Crypto API + per-SoC HW accelerators.              | v0.1 surface; impl v0.3 |
| Multi-proc IPC   | `alp/mproc.h`        | Zephyr `mbox_*` (MHU on Alif), `hwsem_*`, shared-memory regions. | v0.1 surface; impl v0.3 |

### Peripherals: how a block resolves to a backend

Each block in `alpCaner/alp-studio` declares the SDK API it needs in
its manifest (`interfaces.provides`).  The studio's deterministic pin
allocator picks peripheral instances per block and emits codegen that
calls into `<alp/peripheral.h>`.  Block-side driver C files include
ALP SDK headers and consume `alp_i2c_t`, `alp_gpio_t`, etc.

The SDK's job is to take the resolved instance identifier (the
`bus_id`/`pin_id` field) and bind to the right vendor driver, picking
the right backend dir at build time:

```
alp_i2c_open(cfg)
        │
        ▼
   src/<os>/i2c.c             # picks the OS path
        │
        ▼
   vendors/<som>/i2c.c        # picks the vendor HAL call
        │
        ▼
   vendor HAL  →  CMSIS  →  silicon
```

`src/common/` holds the OS-agnostic glue (status-code translation,
parameter validation, opaque-handle allocation), so each OS backend
stays small.

## Versioning

- **Semver.** v0.1.0 ships the public-header surface above and the
  Zephyr-on-AEN peripheral implementation needed for the three v0
  alp-studio blocks (button+LED, SSD1306 OLED, LSM6DSO IMU).
- Minor bumps add libraries; major bumps reserve breaking ABI changes.
- The Zephyr-module path and the plain-CMake `add_subdirectory()` path
  both honour the same SoVersion.

## Quality bar

- Public headers are C99-compatible with Doxygen comments.
- Every public function has at least one Unity / ztest test under `tests/`.
- CI builds three matrices: AEN-Zephyr (real), AEN-baremetal
  (stub OK at v0.1), V2N-Yocto (stub OK at v0.1).
- No GPL dependencies.  Apache-2.0 / MIT / BSD only.

## Why this wrapper exists (despite Zephyr already abstracting vendors)

A common question on first contact with the SDK: "Zephyr already
hides Alif HAL vs. Renesas FSP vs. NXP MCUXpresso below its driver
classes — why add another wrapper on top?"

The answer is that **vendor-driver diversity within Zephyr is not the
problem the SDK solves**.  The SDK's layer earns its keep on five
fronts that Zephyr does *not* cover:

1. **OS portability, not just vendor portability.**  The SDK pivots
   across **three OS targets** (Zephyr / Yocto / baremetal).  An app
   that targets `<alp/i2c.h>` recompiles unchanged on AEN-Zephyr and
   V2N-Yocto.  An app written against `i2c_*` does not — Yocto
   doesn't have `i2c_*`, it has `/dev/i2c-N`.  This is the central
   justification for the wrapper.
2. **Studio codegen target.**  alp-studio's pin allocator emits C
   that calls a fixed API regardless of which OS the SoM uses.
   Without the ALP wrapper, studio codegen would fork per-OS.
3. **Opaque studio-resolved IDs.**  Zephyr's API takes a
   `const struct device *` (a DT label literal).  The studio
   resolves `bus_id` / `pin_id` / `channel_id` from the e1m-spec
   chain and hands the SDK an integer.  That integer makes apps
   portable across SoMs at the **C-source level** — without the
   wrapper apps would need DT-label substitution at build time.
4. **ABI stability for v1.0.**  Zephyr's APIs can change across LTS
   lines.  The ABI snapshot at `docs/abi/v0.1-snapshot.json` is the
   boundary that lets us absorb Zephyr churn without breaking apps.
   v1.0 commits to that surface for 24 months.
5. **Co-location of chip drivers + higher libraries.**  LSM6DSO,
   SSD1306, BME280, plus `<alp/audio.h>` / `<alp/camera.h>` /
   `<alp/ble.h>` etc. — these don't belong in Zephyr proper.  They
   belong above it.  The SDK is that layer.

**Design constraint that follows.**  The wrapper *should stay thin*
over Zephyr's API.  Reimplementing what Zephyr already does (DMA
schedulers, custom driver classes) adds bug surface without
portability gain.  The ALP value is *studio-friendly + OS-portable +
ABI-stable*, not *vendor-diversity over Zephyr*.

## Capability validation

Every E1M-conformant SoM ships with a different peripheral inventory
— Alif Ensemble E3 has a 24-bit ADC plus three 12-bit ADCs, an Alif
E7 has the same, while NXP i.MX 93 tops out at 12 bits.  Apps that
declare a 16-bit ADC config must fail predictably when run on a SoC
that can't satisfy it.

The SDK's three-layer error mechanism:

1. **Studio codegen (best place).**  alp-studio reads the active
   SoM's metadata (`metadata/socs/<vendor>/<family>/<part>.json`)
   and rejects block configurations that exceed the SoC's
   documented caps **at codegen time** — the cheapest layer to
   enforce.
2. **SDK runtime open() (defensive).**  Each `alp_*_open` validates
   its config against the active SoC's compile-time capability
   table (`include/alp/soc_caps.h`, generated from the metadata
   files by `scripts/gen_soc_caps.py`).  A 16-bit ADC request on a
   12-bit SoC returns NULL with `alp_last_error()` =
   `ALP_ERR_OUT_OF_RANGE`, before any I/O.
3. **SDK runtime call() (last-line).**  Zephyr's `-ENOTSUP` is
   mapped to `ALP_ERR_NOSUPPORT` via the wrapper's errno table, so
   capabilities only knowable at use time still surface cleanly.

The compile-time capability table is selected by Kconfig — the
studio-generated build sets `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y` (or
the analogue for the active SoM) and the matching `ALP_SOC_*_COUNT`
/ `ALP_SOC_*_MAX_*` macros activate.  When no SoC is selected the
macros default to `UINT16_MAX` so capability checks pass through —
i.e. validation is opt-in via Kconfig, never a build-breaking
addition.

Apps that bypass the studio (hand-written firmware) can read
`alp_last_error()` after a failed `alp_*_open` to learn whether the
failure was a config-out-of-range, a pool-exhaustion, a NULL device,
or an underlying driver error.  The diagnostic is thread-local so
concurrent open() calls don't clobber each other.

## Consumers of this SDK

The SDK is the bottom of a small dependency stack.  Three repositories
link against it directly; everything else reaches it transitively
through them.

```
+-----------------------------------------------------------+
|  End-user application                                     |
|  Hand-written firmware OR an alp-studio export            |
+-----------------------------------------------------------+
                |                       |
                v                       v
+-----------------------------+  +-------------------------+
|  alpCaner/alp-studio        |  |  Hand-written user code |
|  (block library + visual    |  |  (Zephyr application,   |
|  programmer + codegen)      |  |  Yocto application)     |
+-----------------------------+  +-------------------------+
                |                       |
                |   #include <alp/...>  |
                v                       v
+-----------------------------------------------------------+
|  alpCaner/alp-sdk    THIS REPO                            |
+-----------------------------------------------------------+
                |
                v
+-----------------------------------------------------------+
|  Vendor HALs / Zephyr / Yocto                             |
+-----------------------------------------------------------+
```

### alp-studio integration contract

`alpCaner/alp-studio` is the visual programmer that reads block
manifests and generates Zephyr-app source code that calls into this
SDK.  The integration contract is:

- The studio's pin allocator produces opaque `bus_id` / `pin_id`
  integers per the chain in [`e1m-pinout.md`](e1m-pinout.md).
- The codegen emits calls into `<alp/peripheral.h>`,
  `<alp/chips/...>.h`, and (for camera / IoT / GUI blocks) the
  per-library `<alp/...>` headers.
- The studio reads `metadata/socs/<vendor>/<family>/<part>.json` to
  resolve a project's `soc_ref` and tailor codegen to the active
  chip's peripheral inventory.
- The studio does **not** read `src/` or `chips/<part>/`.  Those are
  implementation; the studio only sees the public headers and the
  metadata directory.

When a v0.x release ships, the studio's `library/` directory pins to
the matching SDK tag in `west.yml`.

### Zephyr-application consumption

A hand-written Zephyr application uses the SDK as a Zephyr module:

```yaml
# west.yml
manifest:
  projects:
    - name: alp-sdk
      url: https://github.com/alpCaner/alp-sdk
      revision: v0.1.0
      path: modules/alp-sdk
```

```kconfig
# prj.conf
CONFIG_ALP_SDK=y
CONFIG_ALP_SDK_CHIP_LSM6DSO=y
```

```c
// src/main.c
#include <alp/peripheral.h>
#include <alp/chips/lsm6dso.h>
```

### Yocto consumption (v0.4+)

The Yocto path is symmetric: `meta-alp` ships a recipe that builds
the SDK's `src/yocto/` backend as a shared library, exposes the
public headers under `/usr/include/alp/`, and wires `pkg-config`
data so application recipes can `inherit pkgconfig` and depend on
`alp-sdk`.

## Sources of truth (do not duplicate)

- HW pinout — [`alpCaner/e1m-spec`](https://github.com/alpCaner/e1m-spec)
  (v1.0).  See [`docs/e1m-pinout.md`](e1m-pinout.md) for how the
  spec, the per-SoM manifests, and the SDK's opaque `bus_id` /
  `pin_id` integers all relate.
- Per-SoM peripheral exposure — `alpCaner/alp-studio` →
  `library/_soms/<id>/manifest.json`.
- AI accelerator runtimes (Ethos-U `vela`, Renesas DRP-AI translator) —
  separate vendor repos.
- Zephyr board files for ALP modules — `alpCaner/alp-zephyr-modules`
  (TBD).

## Non-goals

- **Not a HAL.** Vendor HALs are wrapped, not replaced.
- **Not a board-file collection.** Zephyr boards live in
  `alp-zephyr-modules`.
- **Not the studio.** Chat, allocator, model pipeline, and fab routing
  stay in alp-studio.
- **Not LVGL itself.** Upstream LVGL is included; we ship a config and
  integration only.
