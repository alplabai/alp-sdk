<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 09: `board.yaml` deep dive

**Target audience:** developers who've shipped one ALP SDK build
and want to understand every knob in `board.yaml`, including
the custom-board flow.

**Prerequisites:** Tutorial [01](01-first-build.md) completed.

**Outcome:** confident hand-authoring of `board.yaml` for any
SoM / board combination, including custom boards.  Understand
when each block is required, when the defaults suffice, and how
the loader translates each block into backend config.

**Time:** 30–45 minutes.

---

## The schema in one paragraph

`board.yaml` is the **single declarative file** that says what a
firmware project targets.  Every backend's config -- Zephyr's
`alp.conf`, plain-CMake `-D` flags, Yocto's `local.conf` -- is
**derived** from it by `scripts/alp_project.py` +
`scripts/alp_orchestrate.py`.  The schema lives at
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json);
this tutorial walks every top-level block.

## Required vs optional

Required (the loader rejects a `board.yaml` missing any of these):

- `som.sku`
- `cores:` (mapping of core_id → `{ os, ... }`); every non-`off`
  core needs at least `os:` + (for Zephyr / baremetal) `app:`
- One of:
  - top-level `name:` plus `populated:` / `e1m_routes:` (inline mode,
    customer projects), OR
  - top-level `preset:` (SDK-internal shortcut used by the EVK
    demos at `examples/`)

Everything else is optional.  Adding any per-core or top-level
block adds capability; omitting it falls back to defaults pulled from
the SoM preset's `topology:` block.

## Block-by-block walkthrough

### `som`

```yaml
som:
  sku: E1M-AEN701          # required
  hw_rev: r1               # optional; defaults to default_hw_rev in the family
```

`sku` resolves to a preset at
`metadata/e1m_modules/E1M-<MPN>.yaml`.  The SDK ships presets
for every released MPN:

| Family            | MPNs (paste any into `som.sku`)                            |
|-------------------|------------------------------------------------------------|
| Alif Ensemble     | `E1M-AEN301`, `AEN401`, `AEN501`, `AEN601`, `AEN701`, `AEN801` |
| Renesas RZ/V2N    | `E1M-V2N101`, `V2N102`                                     |
| RZ/V2N + DEEPX    | `E1M-V2M101`, `V2M102`                                     |
| NXP i.MX 93       | `E1M-NX9101`                                               |

`hw_rev` cross-checks against the family's `hw-revisions.yaml`.
If the customer's SDK version is older than the rev's
`min_sdk_version`, the loader exits with code 3.  Always pin
`hw_rev` for production; omit for bring-up convenience.

### Board declaration

The board the firmware targets is declared at the **top level**
of `board.yaml`, with two mutually-exclusive modes:

**Inline (the customer path).**  Self-contained, no indirection
-- write your board out in the same file:

```yaml
name: my-sensor-board       # required when going inline
description: "..."          # optional, free-form
hw_rev: r1                  # optional board hardware revision

populated:                  # chips populated on this board
  bmi323:   true
  ssd1306:  true
  rv3028c7: true

e1m_routes:                 # E1M-pad -> board-side macro
  gpio:
    - { e1m: E1M_GPIO_IO15, macro: PIN_BMI323_INT1,
        doc: "BMI323 INT1 (data-ready / motion / FIFO)." }
  buses:
    - { e1m: E1M_I2C0, macro: I2C_BUS_SENSORS,
        doc: "Shared sensor bus." }
  pwm:
    - { e1m: E1M_PWM3, macro: PWM_LED_RED,
        doc: "Status LED red channel." }
```

Each `populated.<name>: true` enables the matching
`CONFIG_ALP_SDK_CHIP_<NAME>=y` (or
`CONFIG_ALP_SDK_BLOCK_<NAME>=y` for the SDK-level helpers
`button_led` / `pdm_mic`).  Bus addresses and pin assignments
live in the chip-driver metadata under
`metadata/chips/<name>.yaml`; `e1m_routes:` is the canonical
source for E1M-pad → board-side macro names that hand-written
firmware references (`PIN_*`, `I2C_BUS_*`, `PWM_*`).
`scripts/gen_board_header.py` reads `e1m_routes:` and emits
`include/alp/boards/alp_<name>_routes.h` automatically.

For custom pad mappings layered on top of the SoM-provided
defaults, emit a starter devicetree overlay via
`python3 scripts/alp_project.py --input board.yaml --emit dts-overlay`
and hand-edit from there.

**Preset (SDK-internal shortcut).**  The 41 example projects in
this repo all target the EVK + X-EVK, so they share a single
shared board definition via `preset:`:

```yaml
preset: e1m-evk             # or e1m-x-evk
```

The resolved preset lives at `metadata/boards/<name>.yaml` and
supplies `name`, `populated`, `e1m_routes`, `default_hw_rev`,
and `hw_revisions` wholesale.  Customer projects don't need this
-- write your board inline as above so the file is
self-contained -- but you can use a shared preset if your fleet
of firmware projects all target one physical board.

The SDK ships these shared presets:

- `e1m-evk` -- 35×35 reference board for AEN + N93.
- `e1m-x-evk` -- 45×65 reference board for V2N + V2N-M1.
- `custom-example` -- copy-friendly template (use `cp` on the
  file contents into your own project's `board.yaml`).

When `preset:` is set, you cannot ALSO carry inline
`populated:` / `e1m_routes:` -- the schema rejects mixing.

### `pins` (optional E1M-pad usage list)

```yaml
pins:
  - { e1m: E1M_GPIO_IO4, macro: EVK_PIN_ENCODER_SW, doc: "user button" }
  - { e1m: E1M_PWM3,     macro: EVK_PWM_LED_RED,    doc: "red status LED" }
  - E1M_I2C0                                                       # bare form OK
```

Optional top-level array.  Names the E1M pads / peripheral
instances the project actively touches.  Most useful in preset
mode, where the resolved board defines the full wiring but
readers can't tell which subset this particular firmware uses
without diving into the source.  Listing them here surfaces the
project's hardware usage in one place.

Each entry is either a bare E1M pad name (e.g. `E1M_GPIO_IO4`)
or a `{e1m, macro?, doc?}` mapping that pins the C macro the
source actually references plus a one-line label.  Bare-string
and object entries can mix in the same list.

The loader cross-checks every entry against the resolved board's
`e1m_routes:` block: the `e1m` must exist, and when `macro:` is
supplied it must match the board's macro for that pad.  Typos
and stale macro names error out at validate time.

### Pin direction (NOT in `board.yaml`)

`board.yaml` describes the **wiring** (pad ↔ feature) and
**board-static electrical facts** (`active_low`, `pull`,
`debounce_ms`); it does NOT describe pin direction.

Pin direction is a per-app runtime choice -- set at the
`alp_gpio_open()` call site by the firmware:

```c
alp_gpio_t *btn = alp_gpio_open(EVK_PIN_ENCODER_SW,
                                ALP_GPIO_INPUT | ALP_GPIO_INT_EDGE_FALLING);
alp_gpio_t *led = alp_gpio_open(EVK_PWM_LED_RED, ALP_GPIO_OUTPUT);
```

For peripheral use (UART / SPI / I²C / PWM / …) the
`alp_<class>_open()` call muxes the pads to the right function
automatically; the app doesn't say "TX is output" by hand.

The reason direction stays in the firmware: the same pad can
have multiple legitimate directions in different apps.  The
drone-autopilot uses `E1M_PWM3` as a PWM output driving an ESC
channel; gpio-button-led uses the same pad as a GPIO output
driving the red status LED.

### `cores.<id>.os` (per-core runtime)

```yaml
cores:
  m55_hp:
    os: zephyr       # zephyr | yocto | baremetal | off (rarely written)
    app: ./src
  m55_he:
    os: "off"        # explicit opt-out for a peer core
```

Per-core runtime selector.  **Usually omitted:** each core's
natural runtime is baked into the SoM preset's `topology:` block
(Cortex-M → zephyr, Cortex-A → yocto), so the loader fills it in
automatically.  Write `os:` only to override:

- `os: "off"` -- skip the slice entirely (no app required).  The
  peer core stays at reset; useful when an example demos a
  single core and you want the build fast.
- `os: baremetal` -- hand-written firmware on a core that
  normally runs Zephyr.
- `os: yocto` / `os: zephyr` -- only meaningful when you need to
  override the topology default.  The SoM preset is the source
  of truth otherwise.

The loader validates the pair against the SoM's `topology:`
declaration -- `os: yocto` on a Cortex-M core is rejected with a
clear error.

### `cores.<id>.peripherals`

```yaml
cores:
  m55_hp:
    app: ./src
    peripherals:
      - i2c
      - spi
      - pwm
      - adc
```

List the Zephyr peripheral subsystems this slice's app uses.
Each entry becomes `CONFIG_<X>=y` in the generated `alp.conf`
for that slice.  Per-core under v2 -- different cores in a
heterogeneous build can declare different peripheral sets.
Allowed values (exactly the schema enum):

```
adc, can, counter, emmc, ethernet, flash, gpio, i2c, i2s,
pwm, rtc, sensor, spi, uart, usb, watchdog
```

Higher-level concerns ride other paths: audio is composed from
`i2s` + a codec chip driver; inference / IoT / BLE / security /
mproc / DSP / GPU2D have dedicated `<alp/...>` surfaces wired
in via `libraries:`, `iot:`, `inference:`, or by enabling the
chip-driver under `board.populated:`.

Omit the block entirely if your slice uses no peripherals
(rare; most apps need at least `gpio`).

### `cores.<id>.inference`

```yaml
cores:
  m55_hp:
    app: ./src
    inference:
      default_arena_kib: 512    # per-model scratch arena; the only knob.
```

App-level inference tuning, scoped per-core.  There is **no `backend:` field** —
the dispatcher set is silicon-determined from the SoM preset's
`capabilities:` block.  The SDK compiles in every NPU the SoM
declares (Ethos-U on AEN + N93, DRP-AI on V2N + V2M, DEEPX on
V2M) plus the TFLM CPU fallback as universal.  Apps pick which
to run **per-handle at runtime** via `alp_inference_open(.backend = …)`
— V2M101 can run independent models on DRP-AI3 and DEEPX
DX-M1 concurrently this way.  See
[`docs/tutorials/16-inference-mobilenet.md`](16-inference-mobilenet.md).

### `cores.<id>.iot`

```yaml
cores:
  m55_hp:
    app: ./src
    iot:
      wifi: true
      mqtt: true
      tls:  true
      ble:  false
```

Toggles the connectivity subsystems for this slice.  Each
`true` pulls in the matching backend (Wi-Fi stack, MQTT client,
mbedTLS / OpenSSL, BLE host).  Per-core under v2 -- a
heterogeneous build typically lights up `wifi/mqtt/tls` on the
A-class slice and leaves the M-class slice quiet.  TLS pinning
lives in application code -- see Tutorial [11: MQTT-TLS publish](11-mqtt-tls-publish.md).

### `cores.<id>.libraries`

```yaml
cores:
  m55_hp:
    app: ./src
    libraries:
      - etl
      - fmt
      - lvgl
```

Per-core under v2 -- different slices can pull in different
library sets (lvgl on the UI core, cmsis_dsp on the DSP core).
User-facing libraries the SDK threads through to the build.
Apps use these through their **native API** -- no
`<alp/...>` wrapping.  Allowed values (with their natural
APIs):

- `etl`         -- ETL (Embedded Template Library)
- `fmt`         -- {fmt}
- `nlohmann_json` -- JSON for Modern C++
- `doctest`     -- doctest unit-test framework
- `lvgl`        -- LVGL
- `mbedtls`     -- MbedTLS (exposed alongside `<alp/security.h>`)
- `cmsis_dsp`   -- CMSIS-DSP (exposed alongside `<alp/dsp.h>`)
- `littlefs`    -- LittleFS
- `tflite_micro`, `pid`, `modbus`, `nanopb`, ... -- see the
  full enum in
  [`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json).

### `cores.<id>.extra_libraries` -- open-set escape hatch (v0.6)

The curated `libraries:` enum is closed; `extra_libraries:` is the
open-set escape hatch for libraries the SDK doesn't curate (a
one-off vendor SDK, a research-only dep, a library on its way
into the curated set).  Each entry MUST declare exactly one of
`kconfig:` (inline fragment) or `profile:` (`hw-backends.yaml`-style
file):

```yaml
cores:
  m55_hp:
    app: ./src
    libraries:
      - mbedtls            # curated, closed enum
    extra_libraries:
      - name: zforce       # one-off vendor SDK
        include_path: third_party/zforce/include
        kconfig:
          - CONFIG_ZFORCE=y
      - name: mycrypto     # per-silicon backend selection
        profile: third_party/mycrypto/hw-backends.yaml
```

Loader rules: exactly-one of `kconfig`/`profile`, names globally
unique across every core's `extra_libraries:`, no collisions with
the curated `libraries:` enum, and `profile:` paths must resolve
to a real file.  See `docs/board-config.md` §`extra_libraries:`
for the full reference + the cross-field validator pass.

### `chips` (top-level, opt-in chip drivers)

```yaml
chips:
  - lsm6dso
  - bmp581
```

Top-level **array** of chip-driver names the application links
directly via `<alp/chips/<name>.h>`.  Each entry enables the
matching `CONFIG_ALP_SDK_CHIP_<NAME>=y`.  Shared across cores
(project-wide chip population).

Two ways a chip ends up enabled in the build:

1. **Opt-in** -- the customer lists the driver here.  Used for
   chips populated on a custom board or hand-soldered onto a
   stock one.
2. **Auto-enabled from `on_module:`** -- the SoM preset's
   `on_module:` block declares every chip soldered onto the
   module at fab time (CC3501E + OPTIGA on AEN, GD32G553 on
   V2N, ...).  The orchestrator auto-enables those Kconfig
   gates without the customer writing anything.  This is why
   `CONFIG_SPI=y` lights up on AEN even when an app doesn't
   declare `peripherals: [spi]` -- CC3501E forces it (see
   capability-validation note in
   [`docs/portability-matrix.md`](../portability-matrix.md)).

### Sparse capabilities: SoC defaults + SoM extensions

The SoM preset's `capabilities:` block is **sparse** -- it
declares only what the SoM adds on top of its SoC's defaults.
The orchestrator merges `metadata/socs/<silicon>.json`'s
`capabilities:` with the SoM preset's overrides at config time.
Examples:

- `ethos_u55_count: 2` is a silicon-determined fact on AEN701;
  the SoM preset doesn't have to repeat it.
- `drp_ai: true` is silicon-determined on every V2N; not in the
  SoM YAML.
- `deepx_dx: true` IS in the V2M preset (DEEPX is on-module via
  PCIe, not silicon-default for the V2N SoC).

You never write `capabilities:` in your own `board.yaml`.  The
merged dict surfaces to backends (NPU dispatchers, peripheral
drivers) at build time.

### `diagnostics`

```yaml
diagnostics:
  log_level: info   # debug | info | warn | error
```

Sets the SDK's log verbosity.  Maps to Zephyr's
`CONFIG_LOG_DEFAULT_LEVEL` + the Yocto-side `LOG_LEVEL` env var.

### `storage` (persistent partitions)

```yaml
storage:
  - { name: settings,        fs: littlefs, size_kib: 64,  mount: /lfs/settings,
      flash_device: mram_main }
  - { name: app_data,        fs: littlefs, size_kib: 128, mount: /lfs/app,
      flash_device: mram_main }
  - { name: mcuboot_scratch, fs: raw,      size_kib: 32,
      flash_device: mram_main }
```

Each entry declares one fixed partition.  `flash_device:` resolves
against either the SoM's `memory_map:` regions (auto-derived from
the SoC variant when not overridden) or `on_module.ospi_memories:`
keys (when the SoM ships with external OSPI flash).

The orchestrator allocates partitions bottom-up within each device,
**name-sorted, page-aligned to 4 KiB**, so addresses stay byte-stable
across rebuilds -- the property OTA images depend on.  In the example
above the resolver yields:

| Partition          | Offset | Size  | DT label        |
|--------------------|--------|-------|-----------------|
| `app_data`         |   0    | 128 K | `&mram_main`    |
| `mcuboot_scratch`  | 128 K  |  32 K | `&mram_main`    |
| `settings`         | 160 K  |  64 K | `&mram_main`    |

(name-sorted, hence `app_data` before `mcuboot_scratch` before
`settings`).  Override the allocator with `offset_kib: <N>` on any
entry; the resolver checks page alignment + sibling overlap and
projects collisions as `status: blocked` in the manifest with a
clear reason.

The build absorbs three outputs per project:

* `build/generated/dts-partitions.dtsi` -- a DTS overlay decorating
  `&<dt_label>` with a `partitions { compatible = "fixed-partitions"; ... }`
  child node.  Apps reach individual partitions via Zephyr's
  `FIXED_PARTITION_ID(<name>_partition)`.
* `CONFIG_FILE_SYSTEM=y` + the per-fs Kconfig (`LITTLEFS` /
  `FAT_FILESYSTEM_ELM` / `FILE_SYSTEM_EXT2`) + per-partition
  `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y` in each Zephyr slice's
  `alp.conf`.
* Optional `--emit storage-mounts-c` produces a static
  `fs_mount_t alp_storage_mounts[]` array the app iterates at boot.

Inspect the resolved layout:

```bash
python3 scripts/alp_orchestrate.py --input board.yaml --emit system-manifest \
    | yq '.storage[]'
python3 scripts/alp_orchestrate.py --input board.yaml --emit dts-partitions
```

The loader rejects typoed `flash_device:` references at parse time
with the list of known devices for the project's SoM.  When the
device's size is `TBD` (HW-config still owed), the resolver projects
the entry as `status: blocked` with a reason pointing at the SoM
file owing the value -- so the manifest stays emit-able and CI
sees the gap.

## Worked example: custom IoT board on AEN

A custom board that doesn't match any shipped preset, with
BLE + MQTT-TLS + LVGL display:

```yaml
name: my-iot-board       # inline board: required when no `preset:`

som:
  sku:    E1M-AEN701
  hw_rev: r1

populated:
  bme280:     true
  ssd1306:    true
  button_led: true
# Bus addresses + pad aliases live in chip-driver metadata
# + devicetree overlays; see the `### board` section above
# for the contract.  board.yaml itself stays declarative.

cores:
  m55_hp:
    app: ./src        # os: omitted -- M-cores default to zephyr per topology
    peripherals: [i2c, spi, gpio]
    libraries:   [lvgl, mbedtls]
    inference:   { default_arena_kib: 256 }   # arena tuning only
    iot:
      wifi: true
      mqtt: true
      tls:  true
      ble:  true
  m55_he:
    os: "off"         # E7's second M55 stays dark on this app

diagnostics:
  log_level: debug   # bring-up phase; tighten before release
```

Run `python3 scripts/validate_board_yaml.py --input board.yaml`
to lint before building.  Exit 0 = ok; exit 1 = schema error
(JSON-pointer location in the message); exit 2 = missing
preset; exit 3 = hw_rev incompatible with SDK version.

## What you can't put in `board.yaml`

- **Anything silicon-specific.**  No Alif HAL config, no
  Renesas RA-driver knobs.  Those live in the SoM preset
  (which the SDK owns) or, for hand-overrides, in Zephyr's
  devicetree overlays (which `alp_project.py` emits via
  `--emit dts-overlay`).
- **`inference.backend:` -- there is no such field.**  The
  dispatcher set is silicon-determined from the SoM preset's
  `capabilities:` block.  Apps pick at runtime per-handle via
  `alp_inference_open(.backend = ...)`.  See
  `[[silicon-determined-fields-not-customer-facing]]` and
  [Tutorial 16](16-inference-mobilenet.md) for the
  multi-NPU concurrent-dispatch pattern.
- **`capabilities:` -- SDK-internal.**  Belongs in the SoM
  preset (with sparse SoC-defaulted resolution).  Loader-read
  only.
- **Filesystem paths to host tools.**  `board.yaml` is
  hermetic -- a CI run on a clean machine should consume the
  same file your laptop does.
- **Credentials.**  Wi-Fi PSK, MQTT broker URI, OPTIGA
  device keys are application-owned, not config-file-owned.

## See also

- [`docs/board-config.md`](../board-config.md) -- the schema
  reference (this tutorial is the worked-example companion).
- [`scripts/validate_board_yaml.py`](../../scripts/validate_board_yaml.py)
  -- the customer-side linter.
- [`metadata/templates/board.yaml`](../../metadata/templates/board.yaml)
  -- a heavily-commented template you can copy as a starting
  point.
- Tutorial [12: Mender OTA on Yocto](12-mender-ota.md) (TBD)
  for how `board.yaml`'s OTA-config block threads through to
  meta-alp.
