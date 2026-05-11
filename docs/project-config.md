# Project configuration (`alp.yaml`)

`alp.yaml` is the **single file** that declares what a firmware
project targets: which SoM SKU, which OS backend, which inference
backend, which optional libraries, which connectivity features.
Consumers place one at their app root, fill in the SoM SKU and any
overrides, and the SDK's loader handles the rest.

This document is the design + reference; if you just want to copy
a template and start, see
[`metadata/templates/alp.yaml`](../metadata/templates/alp.yaml).

## Why one file

Pre-`alp.yaml` the user had to track configuration across:

| Path | What it picked |
|------|-----------------|
| `prj.conf` (Zephyr) | `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y` + Kconfig knobs |
| cmake configure args | `-DALP_OS=baremetal -DALP_SOM=aen` |
| `local.conf` (Yocto) | `MACHINE=e1m-x-v2n-m1` + IMAGE_INSTALL |
| Per-component cmake / Kconfig flags | `CONFIG_ALP_SDK_CHIP_LSM6DSO=y`, ... |
| Vendor-library opt-in flags | `CONFIG_ALP_SDK_USE_LWRB=y`, ... |

That's three+ files, three+ syntaxes, and no central source of
truth for "this firmware is for *which* SKU."  Custom-populated
variants (DNI'd a sensor, swapped a barometer) had nowhere clean
to live.

`alp.yaml` collapses all of this into one declarative file.  The
SDK's loader compiles it down to the per-backend native config
formats so the underlying Zephyr / CMake / Yocto plumbing
stays unchanged.

## File location

```
your-app/
├── alp.yaml             # <-- this file
├── CMakeLists.txt
├── prj.conf             # auto-augmented by the loader
└── src/
```

One per project.  Hand-written by the user.

## Schema

The full JSON Schema lives at
[`metadata/schemas/alp-project-v1.schema.json`](../metadata/schemas/alp-project-v1.schema.json).
Top-level fields:

| Field            | Required | What it picks                                                 |
|------------------|----------|---------------------------------------------------------------|
| `schema_version` | yes      | Constant `1` until a breaking format change.                  |
| `som`            | yes      | SoM SKU + per-component / memory overrides.                   |
| `os`             | yes      | `zephyr` / `yocto` / `baremetal`.                             |
| `inference`      | no       | Backend selector + tensor-arena defaults.                     |
| `libraries`      | no       | Optional libraries to make available to app code.             |
| `iot`            | no       | Wi-Fi / MQTT / BLE / TLS feature toggles.                     |
| `diagnostics`    | no       | `alp_last_error()` + log level.                               |

### SoM vs carrier

The schema deliberately separates two concerns that get conflated:

| Block      | What it describes                                                                | When it changes                                                       |
|------------|----------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `som`      | The **module** that mounts on the carrier -- silicon, on-module radio, on-module secure element / RTC / temperature sensor / EEPROM. | Fixed at SoM-fab time.  You can't DNI on-module parts after order; only SoM-variant SKUs differ. |
| `carrier`  | The **carrier board** -- IMUs, barometers, OLEDs, cameras, microphones, speaker amps, current monitors, I/O expanders, etc. | Different per carrier design.  Custom carriers DNI any component; the EVK ships with a default population. |

On E1M-AEN, the on-module parts are: Alif Ensemble silicon, TI
CC3501E (Wi-Fi/BLE), Infineon OPTIGA Trust M, Micro Crystal
RV-3028-C7 RTC, TI TMP112, Onsemi 24C128 EEPROM.  Everything else
on the dev kit (LSM6DSO, BMI323, ICM-42670, BMP581, OLEDs, OV5640,
TAS2563, INA236, ...) is on the **E1M-EVK carrier**, not on the
module.

### `som` block

```yaml
som:
  sku: E1M-AEN701          # required

  overrides:                # rare -- only for custom SoM variants
    secure_element: none    # custom AEN without the OPTIGA Trust M

  memory:                   # custom DRAM / flash populations
    flash_mbit: 65536       # vs the SKU default
```

### `carrier` block

```yaml
carrier:
  name: E1M-EVK             # stock preset, or any unique name for a custom carrier

  populated:                # delta vs the carrier preset's defaults
    lsm6dso: false          # DNI'd on this custom assembly
    tas2563: false          # no speaker amps populated
```

When the loader resolves the file, each `populated.<name>: true`
becomes `CONFIG_ALP_SDK_CHIP_<NAME>=y` in the generated Kconfig
fragment -- enabling the corresponding chip driver in
`chips/<name>/` without you having to touch a separate config
file.

### Stock presets (SDK-shipped)

```
metadata/
├── e1m_modules/
│   ├── aen/
│   │   ├── sku-aen301.yaml      # TBD pending user HW config
│   │   ├── sku-aen401.yaml      # TBD
│   │   ├── sku-aen501.yaml      # TBD
│   │   ├── sku-aen601.yaml      # TBD
│   │   ├── sku-aen701.yaml      # v0.3 worked example
│   │   └── sku-aen801.yaml      # TBD
│   ├── v2n/
│   │   ├── sku-v2n101.yaml      # v0.3 worked example
│   │   └── sku-v2n102.yaml      # TBD
│   ├── v2n-m1/                  # to land alongside V2N-M1 v0.4 bring-up
│   └── imx93/                   # SKU TBD per user HW config
└── carriers/
    ├── e1m-evk.yaml             # 35x35 EVK (AEN / N93)
    └── e1m-x-evk.yaml           # 45x65 EVK (V2N / V2N-M1)
```

v0.3 ships the schema + two worked SKU examples (`sku-aen701.yaml`
+ `sku-v2n101.yaml`) + the two stock carriers.  Remaining SKU
presets fill in alongside the user-supplied hardware configuration
writeup.  Per the project memory note, values not in the silicon
datasheet stay `TBD` until the user supplies them authoritatively.

### `libraries` block (user-facing, no wrapper)

Listed libraries become available on the include path + link
line.  Apps use them through their **native API** -- no
`<alp/...>` wrapping:

```yaml
libraries:
  - etl                     # then in app code:  #include "etl/vector.h"
  - fmt                     #                    fmt::format("x={}", x)
  - nlohmann_json           #                    nlohmann::json::parse(...)
```

This is the "available without a wrapper" model: the SDK helps
you *enable* the upstream library; the library's own
documentation governs how to use it.

## How the loader compiles the file (v0.4)

The loader (`scripts/alp_project.py`, lands in v0.4) reads
`alp.yaml`, resolves the SKU preset, applies overrides, and
emits:

- **Zephyr**: a `build/generated/alp.conf` Kconfig fragment that
  the build appends to `prj.conf`, plus a DTS overlay setting
  the carrier-specific properties.
- **Plain CMake**: a set of `-D` flags injected into the configure
  step (or written to a `CMakeUserPresets.json`).
- **Yocto**: a `local.conf` snippet + `MACHINE` line.

v0.3 ships the schema + templates + first two SKU presets.  The
loader script + per-backend emission lands v0.4.  Until then,
consumers hand-translate `alp.yaml` to the per-backend formats
using this document as the mapping reference.

## Versioning

`schema_version: 1` is the only valid value today.  Breaking
changes bump to `2`; the SDK supports both for at least one
minor cycle so consumers can migrate.  Backward-compatible
additions (new optional fields, new enum values) ship in the
same v1 schema with a CHANGELOG note.

## See also

- [`metadata/templates/alp.yaml`](../metadata/templates/alp.yaml)
  -- the canonical commented template.
- [`docs/recommended-libraries.md`](recommended-libraries.md)
  -- the curated library list `libraries:` draws from.
- [`docs/getting-started.md`](getting-started.md) -- the
  consumer-facing walkthrough.
- [`metadata/schemas/alp-project-v1.schema.json`](../metadata/schemas/alp-project-v1.schema.json)
  -- the authoritative schema.
