# Project configuration (`board.yaml`)

`board.yaml` is the **single file** that declares what a firmware
project targets: which SoM SKU (MPN), which carrier (daughter
board), which OS backend, which inference backend, which optional
libraries, which connectivity features.  Consumers place one at
their app root, fill in their MPN, and the SDK's loader handles
the rest.

## Quick start: minimum-viable `board.yaml`

Paste this into `board.yaml` at your app root, change the `sku`
to your MPN, and you're done -- everything else is inherited from
the SDK's per-MPN preset:

```yaml
schema_version: 1

som:
  sku: E1M-AEN701        # your MPN -- the SDK ships a preset
                          # at metadata/e1m_modules/<MPN>/som.yaml

carrier:
  name: E1M-EVK          # or your own custom carrier name

os: zephyr               # zephyr | yocto | baremetal
```

That's the whole config for a vanilla "E1M-AEN701 on the EVK
running Zephyr" build.  Optional blocks (`inference`, `libraries`,
`iot`, `diagnostics`) add capability on top -- omit them to get
the defaults from the MPN's SoM preset.

Released MPNs the SDK ships SoM presets for (look under
`metadata/e1m_modules/<MPN>/som.yaml`):

| Family            | MPNs (paste any into `som.sku`)                                              |
|-------------------|------------------------------------------------------------------------------|
| Alif Ensemble     | `E1M-AEN301`, `AEN401`, `AEN501`, `AEN601`, `AEN701`, `AEN801`               |
| Renesas RZ/V2N    | `E1M-V2N101`, `V2N102`                                                       |
| RZ/V2N + DEEPX    | `E1M-V2M101`, `V2M102`                                                       |
| NXP i.MX 93       | `E1M-NX9101` (production MPN TBD pending HW config)                          |

Stock carriers (paste into `carrier.name`):

| Carrier name  | Form factor   | Hosts                                  |
|---------------|---------------|----------------------------------------|
| `E1M-EVK`     | 35×35 carrier | E1M-AEN family, future E1M-N93 family  |
| `E1M-X-EVK`   | 45×65 carrier | E1M-X V2N family, V2N-M1 family        |

For custom carrier boards, the EVK preset works as the reference
design customers fork -- see "Carrier" section below.

---

This document is the design + reference; if you want a deeper
commented template see
[`metadata/templates/board.yaml`](../metadata/templates/board.yaml).

## Single source of truth

`board.yaml` is the **only** place to configure the firmware.  Every
other config artefact -- `prj.conf` (Zephyr), CMake `-D` args (plain
CMake), `local.conf` + `MACHINE` (Yocto) -- is **derived** from it
by `scripts/alp_project.py`.

Concretely:

- **Don't edit `prj.conf` directly.**  The minimum-correct
  `prj.conf` in a v0.3 alp-sdk app contains exactly one non-empty
  line: `rsource "build/generated/alp.conf"`.  Everything else
  flows from `board.yaml` -> `alp.conf` via the loader.
- **Don't pass extra `-D` flags to `cmake` for SDK options.**  The
  loader emits the right set; passing extra flags risks divergence
  from the declared config.
- **Don't hand-edit `local.conf`'s `MACHINE` / `IMAGE_INSTALL`.**
  Generate them via `--emit yocto-conf` and require the result.

If you find yourself reaching for a hand-edit because `board.yaml`
can't express what you want, file an issue -- the right fix is to
extend the schema, not to bypass it.

### Today's gaps (v0.3 -> v0.4)

`board.yaml` covers the SoM + carrier + OS backend + inference + IoT
features + optional libraries today.  Two remaining gaps where
hand-written config still leaks in, both targeted for v0.4:

1. **`west.yml` module list.**  Optional libraries declared in
   `board.yaml`'s `libraries:` list don't yet auto-pin into the
   workspace's `west.yml`.  v0.4 lands either a generator or a
   `west alp-update` extension command.
2. **Per-test `prj.conf` in `tests/zephyr/<area>/`.**  The
   in-repo test infrastructure still uses hand-written
   `prj.conf` files.  These are SDK-internal (not consumer-
   facing) and stay as-is until the loader handles test-style
   configs in v0.4.

DTS overlays for carrier wiring -- previously gap #1 in this
list -- ship in v0.3.  The loader's `--emit dts-overlay` mode
parses `include/alp/boards/<carrier>.h` and generates the bus
aliases (`alp-i2c<N>`, `alp-spi<N>`, `alp-uart<N>`, `alp-pwm<N>`)
plus a stub `alp,pin-array` with one entry per `EVK_PIN_*`
macro.  Per-pad GPIO bank/index columns remain TBD until the
upstream SoM board files land in `alplabai/alp-zephyr-modules`;
the customer fills those in place without renumbering.

For v0.3, consumers writing apps from scratch should still use
`board.yaml` as the canonical config and treat the three gaps as
short-term hand-overrides.  The migration path to the v0.4 single-
source-of-truth model is purely additive on top of v0.3's schema.

## Why one file

Pre-`board.yaml` the user had to track configuration across:

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

`board.yaml` collapses all of this into one declarative file.  The
SDK's loader compiles it down to the per-backend native config
formats so the underlying Zephyr / CMake / Yocto plumbing
stays unchanged.

## File location

```
your-app/
├── board.yaml             # <-- this file
├── CMakeLists.txt
├── prj.conf             # auto-augmented by the loader
└── src/
```

One per project.  Hand-written by the user.

## Schema

The full JSON Schema lives at
[`metadata/schemas/board-config-v1.schema.json`](../metadata/schemas/board-config-v1.schema.json).
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

### SoM vs carrier (kept deliberately separate)

The schema **separates SoM SKU + carrier into distinct blocks**
and keeps the stock SoM presets under their own directory tree
(`metadata/e1m_modules/<family>/sku-*.yaml`).  This is on purpose:

- The SoM is a tightly-controlled, Alp-released hardware item;
  the SKU preset is authoritative + shared across every customer
  using that part.
- The carrier varies per customer board design; the carrier
  preset is either the stock Alp EVK reference or a customer-
  authored fork.

Keeping them in separate file hierarchies means consumer-authored
carrier presets never accidentally override SoM data, and SoM
preset updates from Alp don't drag carrier opinions with them.

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

### EVK as reference design (custom carriers)

The two stock EVK presets aren't just for the dev kit -- they're
the **canonical reference designs** for customer carrier boards
based on Alp SoMs.  Most production carriers inherit ~80% of the
EVK's chip population (it's the validated baseline Alp ships).
Two ways to consume the reference:

**(a) Reference + override** -- best for small derivatives:

```yaml
carrier:
  name: E1M-EVK             # reference the EVK preset
  populated:                # only list deltas
    tas2563: false          # production board has no speaker amps
    ina236:  false          # no current monitors
    ssd1306: false          # no on-board display
```

The loader merges the deltas over the EVK preset's defaults.
Minimal config, easy to read.

**(b) Fork the carrier preset** -- best for full custom boards:

```bash
# Copy the EVK preset as a starting point.
cp metadata/carriers/e1m-evk.yaml \
   metadata/carriers/my-sensor-board.yaml

# Edit the populated list to reflect your assembly.
$EDITOR metadata/carriers/my-sensor-board.yaml
```

```yaml
# In board.yaml:
carrier:
  name: my-sensor-board
```

A worked example fork lives at
[`metadata/carriers/custom-example.yaml`](../metadata/carriers/custom-example.yaml).
Custom carrier presets can live in the alp-sdk repo (when the
board design ships under Alp), or in the consumer's own repo
(the loader's `--metadata-root` arg accepts an alternate search
path).

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
`<alp/...>` wrapping.  This is intentional: wrapping every
upstream library would be chaos -- different idioms, different
error models, different lifecycles -- and most consumers want
the upstream library as it ships, not Alp's opinion of how it
should look.  For "how do I actually use this in app code" see
the [Using enabled libraries](recommended-libraries.md#using-enabled-libraries-no-wrapper-just-use-them)
section in `recommended-libraries.md` -- one short usage snippet
per Tier-1 library.

```yaml
libraries:
  - etl                     # then in app code:  #include "etl/vector.h"
  - fmt                     #                    fmt::format("x={}", x)
  - nlohmann_json           #                    nlohmann::json::parse(...)
```

This is the "available without a wrapper" model: the SDK helps
you *enable* the upstream library; the library's own
documentation governs how to use it.

#### Compatible without wrapping: library profile headers

"No wrapper" does not mean "no integration."  Every Tier-1
library has compile-time configuration knobs that govern its
behaviour in our environment (exceptions on/off, STL availability,
iostream integration, dynamic allocation policy).  Defaults are
written for desktop builds and aren't always right for embedded
firmware on Cortex-M.

The SDK ships **profile headers** under
[`metadata/library-profiles/<lib>/`](../metadata/library-profiles/)
that pre-tune the upstream library for the SDK's invariants
(no exceptions, no `<iostream>`, no STL on M-class).  When the
loader detects a library in `libraries:`, it adds the matching
profile directory to the include path BEFORE the upstream
library's defaults, so the profile wins.

What this gets you:

| Library         | Profile sets                                                    |
|-----------------|-----------------------------------------------------------------|
| `etl`           | `ETL_NO_STL`, `ETL_NO_EXCEPTIONS`, C++17 target.                |
| `fmt`           | `FMT_HEADER_ONLY=1`, `FMT_USE_IOSTREAM=0`, `FMT_EXCEPTIONS=0`.  |
| `nlohmann_json` | `JSON_NOEXCEPTION=1`, `JSON_USE_IMPLICIT_CONVERSIONS=0`.        |

Consumers who need different settings drop their own profile
header at the app's include root; the loader prefers the app's
profile over the SDK's when both exist.

See
[`metadata/library-profiles/README.md`](../metadata/library-profiles/README.md)
for the full design + per-library notes.

## How the loader compiles the file

`scripts/alp_project.py` reads `board.yaml`, validates against the
schema, resolves the SoM SKU + carrier presets, applies overrides,
and emits one of three formats.  Common workflows below.

### Zephyr -- generated `alp.conf` appended to `prj.conf`

The canonical pattern is to run the loader at configure time and
include the generated fragment from `prj.conf`:

```bash
# At your app root, alongside board.yaml + prj.conf:
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit zephyr-conf \
    --output build/generated/alp.conf
```

```kconfig
# prj.conf -- include the generated fragment.  The build picks
# up CONFIG_* settings from any path on KCONFIG_OVERLAY_CONFIGS,
# or you can rsource the file inline:
rsource "build/generated/alp.conf"
```

For an automated wire-up, drop this into your app's `CMakeLists.txt`:

```cmake
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ALP_PROJECT_CONF ${CMAKE_BINARY_DIR}/generated/alp.conf)
add_custom_command(
    OUTPUT ${ALP_PROJECT_CONF}
    COMMAND ${Python3_EXECUTABLE}
            ${ALP_SDK_PATH}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${ALP_PROJECT_CONF}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
)
add_custom_target(alp_project_conf DEPENDS ${ALP_PROJECT_CONF})
list(APPEND OVERLAY_CONFIG ${ALP_PROJECT_CONF})
```

Auto-regenerates whenever `board.yaml` changes; Zephyr's overlay
mechanism then merges the generated Kconfig over `prj.conf`.

### Plain CMake (baremetal / yocto) -- generated `-D` args

```bash
# Pipe the generated args straight into your configure step:
ARGS=$(python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit cmake-args)
cmake -B build $ARGS .
```

Or wire the loader into a `CMakeUserPresets.json` writer if your
build system already drives presets.

### Yocto -- generated `local.conf` snippet

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit yocto-conf \
    --output build/conf/alp-generated.conf
echo 'require alp-generated.conf' >> build/conf/local.conf
```

### DTS overlay for carrier wiring (v0.3)

`--emit dts-overlay` reads the carrier header at
`include/alp/boards/<carrier>.h`, finds every `#define X
ALP_E1M_<class><N>` macro for the v0.3-scoped classes (I2C, SPI,
UART, PWM, GPIO_IO), and emits a Zephyr `.overlay` declaring:

- One alias per bus channel the carrier wires (`alp-i2c<N> =
  &i2c<N>;`, `alp-spi<N> = &spi<N>;`, `alp-uart<N> = &uart<N>;`,
  `alp-pwm<N> = &pwm<N>;`).  The trailing comment on each alias
  lists every macro in the carrier header that references that
  channel.
- An `alp_pins` node with `compatible = "alp,pin-array"` and one
  `gpios` entry per `EVK_PIN_*` macro that resolves to an
  `ALP_E1M_GPIO_IO<N>`.  Each entry's `<&gpioX Y FLAGS>` triplet
  is a TBD placeholder; the trailing comment carries the macro
  name and the E1M IO index so the customer can fill the columns
  in place without renumbering.

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit dts-overlay \
    --output build/generated/alp.overlay
```

Encoders, cameras, displays, and other non-bus device classes land
in the dts-overlay emitter in v0.4 once the upstream SoM board
files lock the gpio bank/index columns.

### What the loader does NOT yet do (v0.4 follow-ups)

- **Cross-validation against `metadata/socs/*.json`** -- the loader
  trusts the SKU preset's `silicon:` field; it doesn't yet
  validate that requested features (e.g. 16-bit ADC) match the
  SoC's documented caps.  The `<alp/soc_caps.h>` runtime check
  catches mismatches at `_open` time today.
- **First-class `west` integration** -- planned as a custom
  `west alp-build` command that wraps the configure + generate +
  build sequence.

## Versioning

`schema_version: 1` is the only valid value today.  Breaking
changes bump to `2`; the SDK supports both for at least one
minor cycle so consumers can migrate.  Backward-compatible
additions (new optional fields, new enum values) ship in the
same v1 schema with a CHANGELOG note.

## See also

- [`metadata/templates/board.yaml`](../metadata/templates/board.yaml)
  -- the canonical commented template.
- [`docs/recommended-libraries.md`](recommended-libraries.md)
  -- the curated library list `libraries:` draws from.
- [`docs/getting-started.md`](getting-started.md) -- the
  consumer-facing walkthrough.
- [`metadata/schemas/board-config-v1.schema.json`](../metadata/schemas/board-config-v1.schema.json)
  -- the authoritative schema.
