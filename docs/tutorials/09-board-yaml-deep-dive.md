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
**derived** from it by `scripts/alp_project.py`.  The schema
lives at
[`metadata/schemas/board-config-v1.schema.json`](../../metadata/schemas/board-config-v1.schema.json);
this tutorial walks every top-level block.

## Required vs optional

Required (the loader rejects a `board.yaml` missing any of these):

- `schema_version: 1`
- `som.sku`
- `carrier.name`
- `os` (one of `zephyr`, `yocto`, `baremetal`)

Everything else is optional.  Adding a block adds capability;
omitting it falls back to defaults pulled from the SoM preset
and carrier preset.

## Block-by-block walkthrough

### `schema_version`

```yaml
schema_version: 1
```

Locked at `1` until a major breaking schema change requires a
bump.  Per [`docs/release-policy.md`](../release-policy.md),
that won't happen pre-1.0 or in any minor v1.x release.

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
declare everything inline:

```yaml
carrier:
  populated:
    bmi323:   true
    ssd1306:  true
    rv3028c7: true
  i2c_addresses:
    bmi323:   0x68
    ssd1306:  0x3C
    rv3028c7: 0x52
  pin_aliases:
    USER_LED_0: E1M_GPIO_IO5
    BUTTON_0:   E1M_GPIO_IO3
```

See the `custom-example/board.yaml` shipped under
`metadata/carriers/`.

### `os`

```yaml
os: zephyr   # zephyr | yocto | baremetal
```

Selects the OS-pivoted backend.  Tied to the SoM family --
AEN ships Zephyr + baremetal; V2N + V2N-M1 + N93 ship Yocto.
The loader validates the pair: `os: yocto` against
`som.sku: E1M-AEN701` is rejected at validation time.

### `peripherals`

```yaml
peripherals:
  - i2c
  - spi
  - pwm
  - adc
```

List the `<alp/X.h>` subsystems the app uses.  Each entry
becomes `CONFIG_<X>=y` in the generated `alp.conf`.  Allowed
values:

```
i2c, spi, uart, gpio, i2s, pwm, adc, dac, counter, qenc,
can, rtc, wdt, usb, audio, inference, iot, ble, security,
mproc, dsp, gpu2d, power, tmu, camera, storage
```

Omit the block entirely if your app uses no peripherals (rare;
most apps need at least gpio).

### `inference`

```yaml
inference:
  backend:    ethos_u       # auto | cpu | ethos_u | drpai | deepx_dx
  arena_size: 524288        # bytes; default depends on backend
```

Selects the inference dispatcher's default backend.  `auto`
picks based on the SoM (Ethos-U on AEN + N93, DRP-AI on V2N,
CPU fallback otherwise).  Apps that want explicit control
override here.

### `iot`

```yaml
iot:
  wifi:
    enabled: true
  mqtt:
    enabled: true
    tls:     true
  ble:
    enabled: false
```

Toggles the connectivity subsystems.  Each `enabled: true`
pulls in the matching backend.  TLS pinning lives in
application code -- see Tutorial [11: MQTT-TLS publish](11-mqtt-tls-publish.md).

### `libraries`

```yaml
libraries:
  - etl
  - fmt
  - lvgl
```

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
schema_version: 1

som:
  sku:    E1M-AEN701
  hw_rev: r1

carrier:
  populated:
    bme280:     true
    ssd1306:    true
    button_led: true
  i2c_addresses:
    bme280:  0x76
    ssd1306: 0x3C
  pin_aliases:
    USER_LED_0: E1M_GPIO_IO5
    BUTTON_0:   E1M_GPIO_IO3

os: zephyr

peripherals:
  - i2c
  - gpio
  - audio

inference:
  backend: cpu       # don't use Ethos-U on this app

iot:
  wifi:
    enabled: true
  mqtt:
    enabled: true
    tls:     true

libraries:
  - lvgl
  - mbedtls

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
