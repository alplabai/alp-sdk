<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 09: `board.yaml` deep dive

**Target audience:** developers who've shipped one ALP SDK build
and want to understand every knob in `board.yaml`, including
the custom-carrier flow.

**Prerequisites:** Tutorial [01](01-first-build.md) completed.

**Outcome:** confident hand-authoring of `board.yaml` for any
SoM / carrier combination, including custom carriers.  Understand
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
[`metadata/schemas/board-config-v2.schema.json`](../../metadata/schemas/board-config-v2.schema.json);
this tutorial walks every top-level block.

## Required vs optional

Required (the loader rejects a `board.yaml` missing any of these):

- `schema_version: 2`
- `som.sku`
- `cores:` (mapping of core_id → `{ os, ... }`); every non-`off`
  core needs at least `os:` + (for Zephyr / baremetal) `app:`

Everything else is optional.  `carrier.name` is optional but
strongly recommended (without it, the loader can't pull a
populated-chips preset).  Adding any per-core or top-level block
adds capability; omitting it falls back to defaults pulled from
the SoM preset's `topology:` block.

## Block-by-block walkthrough

### `schema_version`

```yaml
schema_version: 2
```

Bumped to `2` in v0.6 when heterogeneous orchestration landed
(top-level `os:` → per-core `cores.<id>.os:`).  The v1 schema is
no longer accepted; see
[`docs/heterogeneous-builds.md`](../heterogeneous-builds.md)
§"Migrating from v1" for the mechanical translation.

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

### `carrier`

```yaml
carrier:
  name: E1M-EVK            # required (or inline `populated`)
  populated:               # optional override on top of the preset
    button_led: true
    bme280:    false
```

`name` resolves to a preset at
`metadata/carriers/<name>/board.yaml`.  The SDK ships:

- `E1M-EVK` -- 35×35 reference carrier for AEN + N93.
- `E1M-X-EVK` -- 45×65 reference carrier for V2N + V2N-M1.
- `custom-example` -- copy-friendly template.

`populated` is an additive override on top of the carrier
preset's populated block.  Use `true` / `false` to opt chip
drivers in / out per the customer's specific assembly.  Each
`true` becomes `CONFIG_ALP_SDK_CHIP_<NAME>=y` in the generated
`alp.conf`.

**Custom carrier without a preset:** drop `name` entirely and
declare the populated chips inline:

```yaml
carrier:
  populated:
    bmi323:   true
    ssd1306:  true
    rv3028c7: true
```

Each `true` enables the matching `CONFIG_ALP_SDK_CHIP_<NAME>=y`
and links the chip driver from `chips/<name>/`.  Bus addresses
and pin assignments live in the chip-driver metadata under
`metadata/chips/<name>.yaml` (resolved against the SoM preset's
`i2c_devices:` block), not in `board.yaml` itself.  For custom
pad mappings, layer a devicetree overlay -- emit a starter via
`python3 scripts/alp_project.py --input board.yaml --emit dts-overlay`
and hand-edit from there.

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
chip-driver under `carrier.populated:`.

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
  [`metadata/schemas/board-config-v2.schema.json`](../../metadata/schemas/board-config-v2.schema.json).

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
   chips populated on a custom carrier or hand-soldered onto a
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

## Worked example: custom IoT carrier on AEN

A custom carrier that doesn't match any shipped preset, with
BLE + MQTT-TLS + LVGL display:

```yaml
schema_version: 2

som:
  sku:    E1M-AEN701
  hw_rev: r1

carrier:
  populated:
    bme280:     true
    ssd1306:    true
    button_led: true
  # Bus addresses + pad aliases live in chip-driver metadata
  # + devicetree overlays; see the `### carrier` section above
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
