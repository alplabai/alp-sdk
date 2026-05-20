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
│    Libraries:  GUI/LVGL · Display · Camera · DSP ·          │
│                IoT · Peripherals                            │
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
├── docs/                            # Architecture, ADRs, board-config, bring-up guides
│   ├── architecture.md              # this file
│   ├── board-config.md              # board.yaml schema reference
│   ├── heterogeneous-builds.md      # per-core fan-out walkthrough
│   ├── os-support-matrix.md         # OS × variant × library status
│   ├── porting-new-som.md           # adding HAL/HW for a new E1M variant
│   └── adr/                         # numbered architectural decisions
├── include/alp/                     # PUBLIC HEADERS — the consumer surface
│   ├── peripheral.h                 # alp_i2c_t, alp_spi_t, alp_gpio_t, alp_uart_t
│   ├── display.h
│   ├── camera.h
│   ├── gui.h                        # LVGL re-export with ALP defaults
│   ├── dsp.h                        # composable DSP pipeline (FFT / FAC / filters)
│   ├── rpc.h                        # framed RPMsg surface; opens with <alp/system_ipc.h>
│   ├── inference.h                  # TFLM / Ethos-U / DRP-AI / DEEPX dispatcher
│   ├── iot.h
│   ├── soc_caps.h                   # GENERATED: per-SoC ALP_SOC_*_COUNT macros
│   ├── e1m_pinout.h                 # E1M-family portable pad constants
│   ├── e1m_x_pinout.h               # E1M-X-family portable pad constants
│   ├── chips/                       # one <alp/chips/<part>.h> per chip driver
│   ├── blocks/                      # <alp/blocks/<name>.h> for SDK-level block helpers
│   └── boards/                      # GENERATED: per-board route headers
├── src/
│   ├── common/                      # OS-agnostic helpers (bit ops, ring buffers, status->str)
│   ├── zephyr/                      # Zephyr-backed implementation
│   ├── baremetal/                   # bare-metal implementation
│   └── yocto/                       # Linux/userspace implementation
├── chips/                           # CHIP DRIVER IMPLEMENTATIONS — one dir per IC
│   ├── lsm6dso/                     # symbols are lsm6dso_* (no alp_ prefix on chip drivers)
│   ├── ssd1306/
│   ├── gd32g553/                    # GD32 supervisor / bridge driver (V2N)
│   ├── cc3501e/                     # TI CC3501E Wi-Fi bridge (Alif Ensemble SoMs)
│   └── ...                          # ~80 driver dirs; see chips/README.md for the index
├── blocks/                          # SDK-LEVEL BLOCK HELPERS — multi-peripheral abstractions
│   ├── button_led/                  # symbols are alp_button_led_* (SDK abstraction, not an IC binding)
│   ├── pdm_mic/
│   └── README.md                    # block-vs-chip rationale (`alp_` prefix vs chip-natural name)
├── vendors/
│   ├── alif/                        # Alif HAL bindings (Ensemble)
│   └── renesas-rzv2n/               # Renesas FSP bindings (RZ/V2N)
├── metadata/                        # ALL HW + LIBRARY METADATA — single source of truth
│   ├── schemas/                     # JSON Schemas (board, board-preset, soc-spec-v1, …)
│   ├── socs/<vendor>/<family>/<part>.json   # silicon datasheets (peripheral counts, caps)
│   ├── e1m_modules/<SKU>.yaml       # SoM presets (`on_module:`, `topology:`, `pad_routes:`)
│   ├── boards/<name>.yaml          # shared board definitions (`populated:`, `e1m_routes:`)
│   ├── library-profiles/<name>/     # per-library HW-accelerator binding tables
│   ├── templates/board.yaml         # customer-facing board.yaml template
│   └── protos/                      # protobuf schemas (mproc framing, …)
├── firmware/                        # PREBUILT HELPER-MCU FIRMWARE BLOBS
│   ├── gd32-bridge/                 # GD32G553 bridge firmware (V2N supervisor)
│   └── cc3501e/                     # TI CC3501E Wi-Fi bridge firmware (AEN)
├── cmake/                           # find_package + Zephyr module helpers
│   └── AlpSdkConfig.cmake.in
├── scripts/                         # CODEGEN + ORCHESTRATION
│   ├── alp_orchestrate.py           # board.yaml → per-core slice fan-out + manifest
│   ├── alp_project.py               # per-slice Kconfig / cmake-args / DTS overlay emit
│   ├── gen_soc_caps.py              # SoC JSONs → include/alp/soc_caps.h
│   ├── gen_board_header.py        # board YAML → include/alp/boards/<board>_routes.h
│   ├── validate_board_yaml.py       # board.yaml schema check
│   ├── validate_metadata.py         # SoC / SoM / board preset schema check
│   └── west_commands/               # `west alp-image`, `west alp-flash`
├── west.yml                         # Zephyr-side manifest
├── zephyr/
│   ├── module.yml                   # makes the repo importable as a Zephyr module
│   └── Kconfig                      # ALP_SDK_* options exposed to Zephyr apps
├── examples/<peripheral>-<demo>/    # hand-written firmware reference apps (~50% comment density)
├── tests/                           # Unity / ztest smoke tests, QEMU + real silicon
├── meta-alp-sdk/                    # Yocto BSP layer (V2N / V2N-M1 / iMX93 SKUs)
└── .github/workflows/               # GitHub Actions workflows
```

The public surface is **only** `include/alp/`.  Anything under `src/`,
`vendors/`, or `cmake/` is implementation; consumers must not include
those headers directly.

### chips/ vs blocks/

The `chips/` and `blocks/` trees look adjacent but mean different
things — both lower-level enough to be confusing on first contact:

| Aspect             | `chips/<part>/`                            | `blocks/<name>/`                                |
|--------------------|--------------------------------------------|--------------------------------------------------|
| Bound to           | One datasheet (LSM6DSO, SSD1306, …)        | A *pattern* over peripherals (button+LED, PDM)   |
| Symbol prefix      | The chip's natural name (`lsm6dso_init()`) | `alp_<block>_*` -- it's an SDK abstraction       |
| Public header path | `<alp/chips/<part>.h>`                     | `<alp/blocks/<name>.h>`                          |
| Kconfig symbol     | `CONFIG_ALP_SDK_CHIP_<NAME>`               | `CONFIG_ALP_SDK_BLOCK_<NAME>`                    |
| Swap-friendly      | No — replacing the IC means a new driver   | Yes — any compliant peripheral plugs in          |

The `alp_` prefix is **reserved** for the SDK's portable abstractions
(`<alp/peripheral.h>`, `<alp/dsp.h>`, `<alp/blocks/...>`); chip
drivers stay on their datasheet-native symbol names so a developer
who reads the chip's reference manual recognises the API.  Full
rationale: `blocks/README.md` at the repo root and the memory
note `[[chip-driver-naming]]`.

## Build orchestration

The build flow runs entirely from a single `board.yaml` v2 file in
the consumer's application directory; everything below the
`<alp/...>` line is derived from there + the metadata tree.  Three
distinct mechanisms cooperate:

### Per-core slice fan-out

`board.yaml` v2 carries a top-level `cores:` block keyed by the
canonical core IDs of the active SoM's SoC (`a55_cluster`,
`m33_sm`, `m55_hp`, `m55_he`, …).  Each entry declares the runtime,
app source, peripherals, libraries, and inference / IoT toggles for
*that* core.  `scripts/alp_orchestrate.py` loads the file, resolves
each entry against the SoM preset's `topology:` defaults, and emits
one **slice** per non-`off` core:

```
board.yaml (`cores:`)
        │
        ▼
   load_board_yaml()              # validate, resolve presets, infer OS per core
        │
        ▼
   {core_id: Slice}               # one Slice per non-off core
        │
        ▼
   Orchestrator.fan_out()         # materialise alp.conf / local.conf / cmake-args
        │                         # per slice under build/<core>-<os>/
        ▼
   build/system-manifest.yaml     # content-addressed; deterministic across rebuilds
```

OS inference defaults are silicon-class driven: Cortex-M cores
default to Zephyr, Cortex-A cores default to Yocto Linux.  The
customer writes an explicit `os:` only when overriding the default
(`os: off` to skip a peer core, `os: baremetal` on a Cortex-M that
normally takes Zephyr).  Yocto-on-A55 + Zephyr-on-M33 on a single
V2N SoM is one `alp_orchestrate.py` invocation, not two.  Full
walkthrough: [`docs/heterogeneous-builds.md`](heterogeneous-builds.md).

### Sparse capabilities flow

Silicon-determined capabilities (`ethos_u55_count`, `drp_ai`,
`neon`, `cau`, …) live as the **defaults** in
`metadata/socs/<vendor>/<family>/<part>.json` under a top-level
`capabilities:` block.  Per-SoM YAMLs declare only the **deltas** —
e.g. an E1M-V2N101 sets `tmu_cordic: true` / `tmu_fft: true` /
`tmu_fac: true` to advertise the GD32G553 bridge's hardware math
units, even though the RZ/V2N silicon itself has no on-die CORDIC/FFT
block.

The loader merges them via `resolve_capabilities()` in
`scripts/alp_project.py`: SoC JSON `capabilities:` is the base layer,
SoM YAML `capabilities:` overlays on top, and SoM-side keys win on
collision (so an add-on chip / bridge can override a silicon
default).  No SoM YAML repeats facts already in the SoC JSON — per
the memory note `[[silicon-determined-fields-not-customer-facing]]`,
every silicon-fixed value has exactly one home.

### on_module: auto-enable

A SoM's preset YAML carries an `on_module:` block that names the
chips physically present on the module (PMIC, RTC, secure element,
Wi-Fi/BLE radio, supervisor MCU, Ethernet PHY, …).  The customer's
`board.yaml` does **not** repeat these — swapping `som.sku:` from
`E1M-V2N101` to `E1M-AEN701` automatically swaps the on-module chip
set with zero edits.

`scripts/alp_orchestrate.py` `_slugs_from_on_module` walks the
`on_module:` block (scalar fields, plus the `i2c_devices:` and
`ospi_memories:` sub-blocks) and the `helper_firmware:` list, then
emits `CONFIG_ALP_SDK_CHIP_<NAME>=y` per chip slug (or
`CONFIG_ALP_SDK_BLOCK_<NAME>=y` for slugs that map to a `blocks/`
helper rather than a `chips/` driver — the `button_led` and
`pdm_mic` exceptions).  The orchestrator also pulls in each chip
driver's required Zephyr subsystems (`CONFIG_I2C=y`, `CONFIG_SPI=y`,
…) via the `_CHIP_SUBSYSTEMS` table in `scripts/alp_project.py`.

Devices marked `assembled: optional` in `i2c_devices:` (DNI on some
builds) are **not** auto-enabled; the customer opts them in via
`board.populated:` instead.

### Generators inventory

The metadata-as-source-of-truth principle (memory note
`[[simplification-unification-principle]]`) means most C headers,
Kconfig fragments, and DTS overlays are generated, not hand-edited.
The active generators are:

| Script                                  | Reads                                                | Writes                                                                         |
|-----------------------------------------|------------------------------------------------------|--------------------------------------------------------------------------------|
| `scripts/alp_orchestrate.py`            | `board.yaml` + SoM preset + SoC JSON + board preset| `build/system-manifest.yaml`, `build/generated/alp/system_ipc.h`, `build/generated/dts-reservations.dtsi`, per-slice `alp.conf` / `local.conf` / `cmake-args.txt` |
| `scripts/alp_project.py`                | same inputs as orchestrator                          | Per-slice emits: `--emit zephyr-conf`, `--emit yocto-conf`, `--emit cmake-args`, `--emit dts-overlay`, `--emit hw-info-h`, `--emit west-libraries`; also `--emit composed-route-table` (JSON SoM × board route-table demonstrator) |
| `scripts/gen_soc_caps.py`               | `metadata/socs/**/*.json`                            | `include/alp/soc_caps.h` (per-SoC `ALP_SOC_*_COUNT` + `ALP_SOC_*_MAX_*` macros) |
| `scripts/gen_board_header.py`         | `metadata/boards/<name>.yaml`                       | `include/alp/boards/alp_<board>_routes.h` (board macro mapping)            |
| `scripts/validate_board_yaml.py`        | `board.yaml`                                         | (validator only — non-zero exit on schema error)                               |
| `scripts/validate_metadata.py`          | `metadata/**/*.{json,yaml}`                          | (validator only)                                                               |

All generated artefacts are byte-stable across rebuilds (deterministic
key ordering, no timestamps, no run IDs) so CI can diff them against
the checked-in copies under `include/alp/` and reject any drift.
Zephyr board files for ALP modules are slated to be generated from
the SoM preset YAMLs by a future `--emit zephyr-board` mode (memory
note `[[zephyr-board-from-yaml]]`); for now they are tracked as a
roadmap item in [`docs/porting-new-som.md`](porting-new-som.md).

## Library design

### Peripheral primitives

All peripheral surfaces below are landed as of v0.6.  Per-row HW
verification status (silicon-validated vs paper-correct) is tracked
in [`docs/test-plan.md`](test-plan.md), not duplicated here.

| Class            | Header              | Backed by Zephyr                              |
|------------------|---------------------|-----------------------------------------------|
| I2C              | `alp/peripheral.h`  | `i2c_*`                                       |
| SPI              | `alp/peripheral.h`  | `spi_*`                                       |
| GPIO             | `alp/peripheral.h`  | `gpio_*`                                      |
| UART             | `alp/peripheral.h`  | `uart_*`                                      |
| PWM              | `alp/pwm.h`         | `pwm_*`                                       |
| ADC              | `alp/adc.h`         | `adc_*` + `adc_dt_spec`                       |
| Counter / Timer  | `alp/counter.h`     | `counter_*`                                   |
| Quadrature decoder | `alp/counter.h`   | `sensor_*` (SENSOR_CHAN_ROTATION)             |
| I2S / SAI        | `alp/i2s.h`         | `i2s_*` + memory slab                         |
| CAN / CAN-FD     | `alp/can.h`         | `can_*` (FD via `CAN_MODE_FD`)                |
| RTC              | `alp/rtc.h`         | `rtc_*`                                       |
| Watchdog         | `alp/wdt.h`         | `wdt_*` + `wdt_install_timeout`               |
| USB              | `alp/usb.h`         | `usb_*` device stack                          |
| Power            | `alp/power.h`       | `pm_*` (Zephyr power management subsystem)    |

See [ADR 0003](adr/0003-peripheral-coverage.md) for why this list and
not others.

### Higher libraries

| Library          | Header(s)            | Backed by                                                      | Status |
|------------------|----------------------|----------------------------------------------------------------|--------|
| Display          | `alp/display.h`      | Zephyr `display_*` (SSD1306 first).                            | v0.1 surface; full impl v0.3 |
| Camera           | `alp/camera.h`       | Zephyr `video_*` API.  V2N MIPI CSI-2 wrapper in v0.2.          | v0.1 stub (NOSUPPORT) |
| GUI/LVGL         | `alp/gui.h`          | Upstream LVGL with an ALP `lv_conf.h`.                         | Header re-export only — no custom widgets |
| DSP              | `alp/dsp.h`          | Composable chain primitives — FIR/IIR/FFT/WINDOW via `alp_dsp_chain_t`; CMSIS-DSP SW fallback when `ALP_HAS_CMSIS_DSP` is set; GD32 FAC/CORDIC HW path on V2N via the bridge.  CMSIS-DSP low-level math (`arm_math.h`) consumed directly from app code — the SDK does not re-export it. | v0.5 surface (Wave-2 DSP); see ADR 0007. |
| IoT              | `alp/iot.h`          | Zephyr `net_*` + MQTT client (AEN); Linux net + libmosquitto (Yocto). | v0.1 surface; Yocto MQTT cleartext + TLS (`mqtts://` via `mosquitto_tls_set`) code complete via libmosquitto (v0.4 prep, `pkg_check_modules`-gated), **broker roundtrip untested** -- see [test-plan.md](test-plan.md); Zephyr Wi-Fi+MQTT v0.4 |
| Audio            | `alp/audio.h`        | Zephyr `audio_dmic` + `i2s_*` chains; ALSA `snd_pcm_*` on Yocto.| v0.1 surface; Zephyr backend v0.2; Yocto ALSA backend code complete v0.4-prep (`pkg_check_modules(alsa)`-gated), real capture/playback gates on `hil-yocto` |
| BLE              | `alp/ble.h`          | Zephyr `bt` host stack (peripheral + central + GATT).           | v0.1 surface; impl v0.3 |
| Security         | `alp/security.h`     | MbedTLS PSA Crypto API (Zephyr) + OpenSSL `EVP_*` (Yocto).      | v0.1 surface; Yocto OpenSSL backend (SHA-256/384/512, AES-128/256-GCM, ChaCha20-Poly1305, `alp_random_bytes`) code complete v0.4-prep with KATs green at `tests/yocto/security_openssl.c`; Zephyr MbedTLS impl v0.3 |
| Multi-proc IPC   | `alp/mproc.h`        | Zephyr `mbox_*` (MHU on Alif), `hwsem_*`, shared-memory regions; placeholder framing helper at `src/common/proto/alp_mproc_frame.{h,c}` (replaced by nanopb-generated codec in v0.4-final). | v0.1 surface; framing scaffolding v0.4-prep; full impl v0.3+ |

### Peripherals: how a block resolves to a backend

Each block in `alplabai/alp-studio` declares the SDK API it needs in
its manifest (`interfaces.provides`).  The studio's deterministic pin
allocator reads the per-SoM pad routes from this repo (the
`pad_routes:` block under `metadata/e1m_modules/<SKU>.yaml`), picks
peripheral instances per block, and emits codegen that calls into
`<alp/peripheral.h>`.  Block-side driver C files include ALP SDK
headers and consume `alp_i2c_t`, `alp_gpio_t`, etc.

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
- CI builds three matrices: AEN-Zephyr (code complete; HIL pending),
  AEN-baremetal (stub OK at v0.1), V2N-Yocto (peripheral stubs
  through v0.3; v0.4 prep on `main` covers core-4 peripherals +
  MQTT cleartext+TLS + ALSA audio + OpenSSL security + Mender
  opt-in -- failure-path ctest green, real-hardware verification
  tracked in [test-plan.md](test-plan.md)).
- Full local verification: [`scripts/test-all.sh`](../scripts/test-all.sh)
  (wraps ctest + twister + clang-format + metadata-validate + Doxygen).
  Coverage map: [`docs/testing.md`](testing.md).
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

## E1M as the portability bound

Per the `alplabai/e1m-spec` standard, every E1M-conformant SoM **SHALL
route a fixed minimum set of peripheral instances** with their primary
functions pinned to specific pads:

- 2 × I²C, 2 × SPI, 2 × UART, 2 × I²S, 2 × PDM, 1 × I³C
- 1 × CAN-FD, 1 × Ethernet, 1 × MIPI CSI, 1 × MIPI DSI
- 8 × PWM (`PWM0..PWM7`), 4 × quadrature decoder (`ENC0..ENC3`)
- 8 × single-ended ADC (`ADC0..ADC7`), 2 × DAC

These minimums are the **portability contract**: an app that uses
`E1M_<CLASS><N>` for `N < E1M_<CLASS>_COUNT` is guaranteed to
work on every conformant SoM.  Higher indices are vendor-specific
extensions — the wrapper accepts them up to the SoC's documented count
(e.g. RZ/V2N's six CAN channels), but apps that use them lose the
"swap the SoM, no software changes" property.

The constants live in [`<alp/e1m_pinout.h>`](../include/alp/e1m_pinout.h)
as `E1M_*_COUNT` macros.  alp-studio's pin allocator enforces the
E1M bound for portable blocks; the SDK's runtime layer enforces only
the SoC-specific bound (so vendor-extension blocks work too).  Three
tiers of validation:

```
              tightest                                          loosest
              ────────                                          ───────
  E1M reservation  <  Studio block declaration  <  SoC count  <  driver array
       (8 PWMs)       (block uses PWM0..PWM3)    (12 timers)    (8 entries)
```

Studio codegen catches block-vs-E1M-vs-SoC mismatches up front; the
SDK's `*_open` catches everything that slipped through (config out of
range against the SoC, DT alias unset, etc.) via the
`alp_last_error()` machinery.

## Capability validation

Every E1M-conformant SoM ships with a different peripheral inventory
— Alif Ensemble E3 has a 24-bit ADC plus three 12-bit ADCs, an Alif
E7 has the same, while NXP i.MX 93 tops out at 12 bits.  Apps that
declare a 16-bit ADC config must fail predictably when run on a SoC
that can't satisfy it.

The SDK's three-layer error mechanism:

1. **Studio codegen (best place).**  alp-studio reads the active
   SoM's metadata from this repo
   (`metadata/e1m_modules/<SKU>.yaml` for the SoM preset, which
   pins the active SoC via `silicon:` + `silicon_variant:`, plus
   `metadata/socs/<vendor>/<family>/<part>.json` for the chip
   datasheet) and rejects block configurations that exceed the
   SoC's documented caps **at codegen time** — the cheapest layer
   to enforce.
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
|  alplabai/alp-studio        |  |  Hand-written user code |
|  (block library + visual    |  |  (Zephyr application,   |
|  programmer + codegen)      |  |  Yocto application)     |
+-----------------------------+  +-------------------------+
                |                       |
                |   #include <alp/...>  |
                v                       v
+-----------------------------------------------------------+
|  alplabai/alp-sdk    THIS REPO                            |
+-----------------------------------------------------------+
                |
                v
+-----------------------------------------------------------+
|  Vendor HALs / Zephyr / Yocto                             |
+-----------------------------------------------------------+
```

### alp-studio integration contract

**alp-studio is one of two first-class consumers of this SDK; the
other is hand-written firmware (see
[`docs/firmware-quickstart.md`](firmware-quickstart.md)).**  Both
call the same `<alp/...>` headers.

`alplabai/alp-studio` is the AI-driven visual programmer that sits
on top of alp-sdk.  It reads block manifests + this repo's metadata
and generates Zephyr-app source code that calls into the SDK.  The
integration contract is:

- The studio's pin allocator produces opaque `bus_id` / `pin_id`
  integers per the chain in [`e1m-pinout.md`](e1m-pinout.md).
- The codegen emits calls into `<alp/peripheral.h>`,
  `<alp/chips/...>.h`, and (for camera / IoT / GUI blocks) the
  per-library `<alp/...>` headers.
- The studio reads `metadata/e1m_modules/<SKU>.yaml` (SoM preset,
  including the `pad_routes:` block added in slice 2) and
  `metadata/socs/<vendor>/<family>/<part>.json` (chip datasheet) to
  resolve a project's active SoM + SoC and tailor codegen to the
  inventory the silicon exposes.  alp-sdk's metadata is alp-studio's
  input, not its output.
- The studio does **not** read `src/`, `chips/<part>/`, or
  `blocks/<name>/`.  Those are implementation; the studio only sees
  the public headers and the metadata directory.

When a v0.x release ships, the studio's `library/` directory pins to
the matching SDK tag in `west.yml`.

### Zephyr-application consumption

A hand-written Zephyr application uses the SDK as a Zephyr module:

```yaml
# west.yml
manifest:
  projects:
    - name: alp-sdk
      url: https://github.com/alplabai/alp-sdk
      revision: main           # pin to a release tag once v0.6.0 ships
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

## Repository boundary (alp-sdk vs alp-studio)

The visual programmer (`alplabai/alp-studio`) and this SDK are
**separate repositories on purpose**.  The single rule that
decides where any new artefact lands is the *dual-use acid test*:

> Would a hand-written-firmware author ever directly use this?
> If no → alp-studio.  If yes (or "both audiences want it") →
> alp-sdk.

With the 2026-05-18 standalone reaffirmation (alp-sdk works
without alp-studio; alp-studio is a consumer on top), more
artefacts now meet the "yes" criterion than earlier drafts
assumed — per-SoM pad maps especially.  Anything the studio's
pin allocator + codegen ingests is data a hand-written-firmware
author can also read, so its home is here.

| Belongs in alp-sdk                       | Belongs in alp-studio                   |
|------------------------------------------|-----------------------------------------|
| `<alp/...>` public headers               | Block manifests (`library/blocks/*`)    |
| Chip metadata (`metadata/socs/*.json`)   | Pin allocator                           |
| SoM presets + pad routes                 | Codegen templates                       |
| (`metadata/e1m_modules/*.yaml`)          | Studio-only Kconfig helpers             |
| Generated capability tables              | Block-level examples (studio-exported)  |
| ABI snapshot tooling                     | ADRs about studio architecture          |
| Per-peripheral examples (hand-written)   | `scripts/gen_block_manifest.py`,        |
| ADRs about API design                    | `scripts/studio_codegen_*` (studio-only)|
| `scripts/abi_snapshot.py`,               |                                         |
| `scripts/gen_soc_caps.py` (dual-use)     |                                         |

The shared infrastructure (chip metadata, ABI tooling) lives in
alp-sdk because that is where it is *generated from*; alp-studio
*consumes* it.  See [ADR 0005](adr/0005-alp-sdk-vs-alp-studio-boundary.md)
for the full rationale and edge-case guidance.

## Sources of truth (do not duplicate)

- HW pinout — [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec)
  (v1.1).  See [`docs/e1m-pinout.md`](e1m-pinout.md) for how the
  spec, the per-SoM pad-routing YAMLs, and the SDK's opaque `bus_id` /
  `pin_id` integers all relate.
- **Per-SoM E1M pad → silicon-pin routing** —
  [`metadata/e1m_modules/<SKU>.yaml`](../metadata/e1m_modules/) in
  this repo (the `pad_routes:` block, introduced in slice 2 of the
  metadata unification work).  Earlier drafts of this doc placed the routes
  in `alp-studio/library/_soms/<id>/manifest.json`; that direction
  was reversed on 2026-05-18 to keep all generator inputs in one
  repo.  alp-studio's pin allocator now reads these YAMLs directly.
- AI accelerator runtimes (Ethos-U `vela`, Renesas DRP-AI translator) —
  separate vendor repos.
- Zephyr board files for ALP modules — `alplabai/alp-zephyr-modules`
  (TBD).

## Non-goals

- **Not a HAL.** Vendor HALs are wrapped, not replaced.
- **Not a board-file collection.** Zephyr boards live in
  `alp-zephyr-modules`.
- **Not the studio.** Chat, allocator, model pipeline, and fab routing
  stay in alp-studio.
- **Not LVGL itself.** Upstream LVGL is included; we ship a config and
  integration only.
