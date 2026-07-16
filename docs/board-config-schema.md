# `board.yaml` schema reference

Field-by-field reference for `board.yaml`: the top-level fields, the
`som`/board split, inline vs. `preset:` board declaration, `pins:`,
pin direction, the EVK-as-reference-design workflow, the SDK-shipped
stock presets, and the `libraries:` block (ADR 0018).

See [`docs/board-config.md`](board-config.md) for the landing page
(quick start, single-source-of-truth model, file location).

## Schema

The full JSON Schema lives at
[`metadata/schemas/board.schema.json`](../metadata/schemas/board.schema.json).
Top-level fields:

| Field            | Required | What it picks                                                 |
|------------------|----------|---------------------------------------------------------------|
| `name`           | inline*  | Board name; required in inline mode, forbidden when using `preset:`. |
| `description`    | no       | Free-form one-line board description.                         |
| `hw_rev`         | no       | Board hardware revision.  Defaults to the preset's `default_hw_rev` (preset mode) or unrevisioned (inline mode). |
| `som`            | yes      | SoM SKU (+ optional `hw_rev`).  Silicon-level facts (memory, on-module components, NPU capabilities) live in the SKU preset and are NOT customer-tunable here. |
| `preset`         | preset*  | SDK-internal: pulls a shared board definition from `metadata/boards/<preset>.yaml`.  Mutually exclusive with inline `populated:` / `e1m_routes:`. |
| `populated`      | inline*  | Chips populated on this board.  Each `true` → `CONFIG_ALP_SDK_{CHIP,BLOCK}_<name>=y`.  Mutually exclusive with `preset:`. |
| `e1m_routes`     | inline*  | E1M-pad → board-side macro routing.  Read by `gen_board_header.py` → `include/alp/boards/alp_<name>_routes.h`.  Mutually exclusive with `preset:`. |
| `pins`           | no       | Optional array naming the E1M pads the project actively uses.  Validated against the resolved board's `e1m_routes:`. |
| `cores`          | yes      | Per-core app + library/peripheral knobs.  Each core's `os:` is optional; the SoM topology supplies the natural runtime per core class (Cortex-M → Zephyr, Cortex-A → Yocto). |
| `ipc`            | no       | Cross-core IPC carve-outs (rpmsg / raw_shmem / mailbox_only). |
| `chips`          | no       | Project-level chip drivers beyond what the board ships.     |
| `libraries`      | no       | Curated third-party libraries (ADR 0018).  One top-level list, each entry a `{name, cores?}` object (or a bare name as shorthand for a project-wide `{name}`), e.g. `[{name: lvgl}, {name: cmsis-dsp, cores: [m33_sm]}]`.  `name` resolves to a manifest under `metadata/libraries/<name>.yaml`; omit `cores:` for a project-wide selection or list core ids to scope it.  The orchestrator emits the per-OS wiring and rejects an incompatible selection at emit time.  See [`libraries` (ADR 0018)](#libraries-project-wide-adr-0018) below. |
| `diagnostics`    | no       | `alp_last_error()` + log level.                               |

*Either `preset:` (preset mode) or inline `name:` + `populated:` +
`e1m_routes:` (customer path).  Both omitted is also fine -- a
headless / inference-only build with no board declaration.

Per-core fields under `cores.<id>` (all optional, all inherit from
the SoM preset's `topology.<id>` when omitted):

| Field          | Notes                                                                                  |
|----------------|----------------------------------------------------------------------------------------|
| `app`          | App source dir.  Required for `os: zephyr` / `os: baremetal`.  For `os: yocto` an app-only slice, pair with `recipe:` -- `app:` is a source path, never a bitbake target. |
| `image`        | Yocto image recipe name (e.g. `alp-image-edge`).  Takes priority over `app:`/`recipe:` when both are set. |
| `recipe`       | Yocto bitbake recipe packaging this slice's `app:` (e.g. `alp-lvgl-dashboard`).  Required for an app-only `os: yocto` slice -- otherwise the plan blocks the slice (`yocto-recipe-missing`) instead of emitting an invalid `bitbake <path>`. |
| `os`           | NOT an OS picker — the runtime is class-derived (M→Zephyr, A→Yocto). Only `off` (skip slice) or `baremetal` (rare) are settable; a cross-class OS is rejected. |
| `peripherals`  | Zephyr subsystem / Yocto package list for this slice.                                  |
| `inference`    | App-level inference tuning (`default_arena_kib` only — backend set is silicon-driven). |
| `iot`          | Wi-Fi / MQTT / BLE / TLS toggles; emitted per OS and wireless provider.                |

`cores.<id>.iot` is an emitted build surface, not a comment-only
declaration.  On Zephyr, `wifi` / `ble` first resolve
`metadata/e1m_modules/<SKU>.yaml` `on_module.wifi_ble`: AEN emits
the exact CC3501E bridge backends, unknown native-radio providers
emit generic Zephyr `wifi_mgmt` / BT-host gates, and Linux-owned
Murata/CYW providers stay off Zephyr.  `mqtt` emits Zephyr's
`mqtt_client` stack and `tls` emits the credential/TLS gates.  On
Yocto, `wifi` / `ble` emit stable userland packages such as
`wpa-supplicant`, `iw`, `wireless-regdb`, and `bluez5`; the
BSP/machine layer remains responsible for provider-specific kernel
modules and firmware.  `mqtt` / `tls` add the matching `alp-sdk`
recipe `PACKAGECONFIG` tokens plus CA certificates.

### OS inference from core type

SoM presets under `metadata/e1m_modules/<MPN>.yaml` no longer
declare `os: zephyr` / `os: yocto` per `topology.<core>` entry --
the field is gone from every released preset and the schema
([`metadata/schemas/som-preset-v1.schema.json`](../metadata/schemas/som-preset-v1.schema.json))
no longer lists it under `topology_entry.required`.  Instead the
loader picks the natural runtime from each core's `cores[].type`
in the matching SoC JSON: `cortex-m*` -> `zephyr`, `cortex-a*`
-> `yocto`, anything else -> `off`.  Helper:
`_default_os_from_core_type()` in
[`scripts/alp_orchestrate/`](../scripts/alp_orchestrate/).

The OS is **not** user-selectable: the runtime follows the core
class, full stop.  A `board.yaml` may only **disable** a core
(`os: off`) or drop it to **no-OS** (`os: baremetal`); selecting the
*other* class's OS — `zephyr` on a Cortex-A, `yocto` on a Cortex-M —
is **rejected by the loader** (`OrchestratorError`).  We support
exactly two OSes — Yocto for Linux, Zephyr for the RTOS — mapped to
the silicon class, not chosen.  (The check lives in the loader, not
the schema, because it's cross-file: board.yaml `os:` vs the SoC
`cores[].type`.)  Custom SoMs ported via
[`docs/porting-new-som.md`](porting-new-som.md) get this for free as
long as their SoC JSON declares core types correctly.

**Querying it (for IDEs / tooling).**  Rather than re-deriving the
M/A → OS mapping, tools read the resolved facts directly:

```bash
python3 scripts/alp_project.py --emit os-topology   # JSON to stdout
```

emits, per resolved core, the `core_type`, the `runtime_class`
(`linux` | `rtos`), the class `default_os`, the `effective_os`,
whether the core is `enabled`, and the per-core `allowed_os` — the
valid dropdown, which *excludes the other class's OS* (so a Cortex-A
shows `[yocto, baremetal, off]`, a Cortex-M `[zephyr, baremetal,
off]`).  That's what an editor's board configurator uses to present
the SDK's selection + the legal overrides, instead of guessing or
offering a cross-class OS.

**Silicon-determined fields never appear in `board.yaml`.**  Inference
backend selection, NPU presence, on-module component populations,
and memory capacities are all dictated by the SoM SKU preset under
`metadata/e1m_modules/<MPN>.yaml`.  For a custom SoM variant (e.g.
an AEN without the OPTIGA Trust M, or with non-stock memory),
create a new SKU preset rather than overriding here — there is no
`som.overrides:` or `som.memory:` block to write into.

### SoM vs board (kept deliberately separate)

The schema **separates SoM SKU + board into distinct blocks**
and keeps the stock SoM presets under their own directory tree
(`metadata/e1m_modules/<family>/sku-*.yaml`).  This is on purpose:

- The SoM is a tightly-controlled, Alp-released hardware item;
  the SKU preset is authoritative + shared across every customer
  using that part.
- The board varies per customer board design; the board
  preset is either the stock Alp EVK reference or a customer-
  authored fork.

Keeping them in separate file hierarchies means consumer-authored
board presets never accidentally override SoM data, and SoM
preset updates from Alp don't drag board opinions with them.

The schema deliberately separates two concerns that get conflated:

| Block                                  | What it describes                                                                | When it changes                                                       |
|----------------------------------------|----------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `som:` (block)                         | The **module** that mounts on the board -- silicon, on-module radio, on-module secure element / RTC / temperature sensor / EEPROM. | Fixed at SoM-fab time.  You can't DNI on-module parts after order; only SoM-variant SKUs differ. |
| Top-level `populated:` + `e1m_routes:` (inline) or `preset:` (shared)  | The **board** -- IMUs, barometers, OLEDs, cameras, microphones, speaker amps, current monitors, I/O expanders, etc. | Different per board design.  Custom boards DNI any component; the EVK ships with a default population. |

On E1M-AEN, the on-module parts are: Alif Ensemble silicon, TI
CC3501E (Wi-Fi/BLE), Infineon OPTIGA Trust M, Micro Crystal
RV-3028-C7 RTC, TI TMP112, Onsemi 24C128 EEPROM.  Everything else
on the dev kit (BMI323, ICM-42670, BMP581, TAS2563, INA236, the
camera mux, ...) is on the **E1M-EVK board**, not on the module.
LSM6DSO and an SSD1306 OLED are not soldered on the EVK -- apps that
want them attach the part to the I2C/Qwiic header and declare it in
their board.yaml `chips:` array.

For Alp Studio and other EDA frontends, `scripts/alp_project.py --emit
carrier-netlist` composes the board and SoM layers into a deterministic
JSON handoff: carrier-facing nets from `e1m_routes:` plus carrier BOM
rows from `populated:` and the chip/block manifests under `metadata/`.
This artifact deliberately excludes SoM-internal parts and is not a
KiCad, Gerber, DRC, or PCB-layout output; downstream tools consume it
to create those files.

### `som` block

```yaml
som:
  sku: E1M-AEN801          # required

  hw_rev: r1               # optional -- defaults to the SKU preset's
                            # `default_hw_rev`.  Validated at build
                            # time against the family hw_revisions
                            # table (see board-config-hardware.md);
                            # at runtime the SDK reads the
                            # rev from the on-module BOARD_ID ADC +
                            # resistor divider and aborts boot on
                            # mismatch.

  overrides:                # rare -- only for custom SoM variants
    secure_element: none    # custom AEN without the OPTIGA Trust M

  memory:                   # custom DRAM / flash populations
    flash_mbit: 65536       # vs the SKU default
```

#### `silicon_variant:` (forward MPN reference, set by Alp)

Each production SoM preset declares a top-level `silicon_variant:`
field naming the exact vendor order code the module is built
around -- `AE302F80F55D5LE` for `E1M-AEN301`, `R9A09G056N44GBG`
for `E1M-V2N101`, etc.  The loader uses it to forward-resolve the
matching `variants[]` entry in
[`metadata/socs/<vendor>/<family>/<part>.json`](../metadata/socs/),
which carries the per-variant MRAM / SRAM / package /
`optional_features` data the build needs.

The reverse path (`alp_module_skus` arrays inside each SoC JSON
variant) stays in place as a fallback for legacy presets that
omit the field, AND for the placeholder `E1M-NX9101` preset which
carries `silicon_variant: TBD` per the no-inventing-values rule.
Resolver: `_resolve_silicon_variant()` in
[`scripts/alp_project.py`](../scripts/alp_project.py).

Customers don't touch this field -- it is Alp-set on the released
preset; consumer's `board.yaml` references the SoM by `som.sku:`
only and the variant is resolved automatically.

### Board declaration

The board the firmware targets is declared at the **top level**
of `board.yaml` in one of two mutually-exclusive modes.

#### Inline mode (the customer path)

Self-contained -- write your board's chip population + E1M-pad
wiring directly in your project's `board.yaml`:

```yaml
name: my-sensor-board       # required; used in alp_<name>_routes.h
description: "..."          # optional
hw_rev: r1                  # optional board hardware revision

populated:                  # chips populated on this board
  lsm6dso: true
  bmi323:  true
  bmp581:  true
  ssd1306: true

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
`button_led` / `pdm_mic` -- the loader picks the right symbol per
slug).  See `blocks/README.md` for the block-vs-chip distinction.

The `e1m_routes:` block is the single editable source of truth
for the board-side C macros hand-written firmware uses
(`PIN_BMI323_INT1`, `I2C_BUS_SENSORS`, `PWM_LED_RED`, …).  Each
entry binds an E1M-standard pad or peripheral instance
(`ALP_E1M_GPIO_IO<N>`, `ALP_E1M_PWM<N>`, `ALP_E1M_I2C0/1` / `ALP_E1M_SPI0/1` /
`ALP_E1M_UART0/1` / `ALP_E1M_I3C0`) to a board-side macro plus optional
`doc:` / `active_low:` / `routes_via:` flags.
[`scripts/gen_board_header.py`](../scripts/gen_board_header.py)
reads the block and emits `include/alp/boards/alp_<name>_routes.h`
with one `#define <MACRO> ALP_E1M_<…>` line per entry.

#### Preset mode (SDK-internal shortcut)

Most example projects under `examples/` target the EVK or X-EVK
(66 do today — 46 on `e1m-evk`, 20 on `e1m-x-evk`), so they share a
single board definition each via the `preset:` field:

```yaml
preset: e1m-evk             # or e1m-x-evk
```

The resolved file lives at `metadata/boards/<preset>.yaml` and
supplies `name`, `populated`, `e1m_routes`, `default_hw_rev`,
and `hw_revisions` wholesale.  When `preset:` is set, top-level
`name:`, `populated:`, `e1m_routes:` are forbidden -- the schema
rejects mixing.

`preset:` is a shortcut for the SDK's own demos; customer
projects don't need it -- the inline form keeps your `board.yaml`
self-contained and grep-able.

#### `pins:` (optional E1M-pad usage list)

```yaml
pins:
  - { e1m: E1M_GPIO_IO4,  macro: EVK_PIN_ENCODER_SW, doc: "user button" }
  - { e1m: E1M_GPIO_PWM3, macro: EVK_PIN_LED_RED,    doc: "red status LED (PWM3 pad driven as GPIO)" }
  - E1M_I2C0                                                       # bare form OK
```

Optional top-level array.  Names the E1M pads the project
actively uses.  Most useful in preset mode, where the resolved
board carries the full wiring but readers can't tell which
subset this firmware touches without diving into the source.

Each entry is either:

- a bare E1M pad name (e.g. `E1M_GPIO_IO4`); or
- a `{e1m, macro?, doc?}` mapping that pins the C macro the
  source actually references plus a one-line label.

The loader cross-checks every entry against the resolved board's
`e1m_routes:` block: the `e1m` pad must exist, and when `macro:`
is supplied it must match the board's macro for that pad
(catches drift if the demo references `EVK_PIN_LED_RED` but the
preset moved it).  Bare-string and object entries can mix in the
same list.

#### Pin direction (NOT in `board.yaml`)

Pin direction is **not** a board declaration.  It's a per-app
runtime choice -- the firmware sets direction with
`alp_gpio_configure()` after opening the pin (the handle comes from
the one-argument `alp_gpio_open(pin_id)`):

```c
alp_gpio_t *btn = alp_gpio_open(EVK_PIN_ENCODER_SW);
alp_gpio_configure(btn, ALP_GPIO_INPUT, ALP_GPIO_PULL_UP);

alp_gpio_t *led = alp_gpio_open(EVK_PIN_LED_RED);
alp_gpio_configure(led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
```

For peripheral use (UART / SPI / I²C / PWM / …) the
`alp_<class>_open()` call muxes the pads to the right function
automatically; the app doesn't say "TX is output" by hand.

The reason direction stays in the firmware: the same physical
pad can have multiple legitimate directions in different apps.
The drone-autopilot uses `ALP_E1M_PWM3` as a PWM output driving an
ESC channel; gpio-button-led uses the same pad as a GPIO output
driving the red status LED.  `board.yaml` describes the
**wiring** (pad ↔ feature), which is universal; direction is a
per-app runtime choice owned by the firmware.

Board-static electrical facts that ARE on the pad regardless of
app -- `active_low`, `pull`, `debounce_ms` -- go in
`e1m_routes:` entries on the board side (or are inherited from
the resolved preset).  Drive strength / slew rate / etc. are
SoC-controller settings (Kconfig), not board-level facts.

### EVK as reference design (custom boards)

The stock EVK definitions aren't just for the dev kit -- they're
the **canonical reference designs** for customer boards based on
Alp SoMs.  Most production boards inherit ~80% of the EVK's chip
population (it's the validated baseline Alp ships).

The customer workflow: copy the EVK YAML content into your
project's `board.yaml` as a starting template, then edit
`name:`, `populated:`, `e1m_routes:` for your assembly.

```bash
# Open the canonical EVK definition.
less metadata/boards/e1m-evk.yaml
```

Paste the `name:`, `populated:`, `e1m_routes:` blocks into your
project's `board.yaml` (alongside the `som:` + `cores:` you
already have), edit `name:` to your board's name, drop chip rows
you don't populate, add the rows for chips you do, edit the
`e1m_routes:` entries to reflect your schematic.  Result: one
self-contained file describing your project.

A worked example of the inline form for a slimmed-down sensor
board derived from the EVK lives at
[`metadata/boards/custom-example.yaml`](../metadata/boards/custom-example.yaml)
-- copy that file's body into your project's `board.yaml` if it's
closer to your starting point than the full EVK.

### Stock presets (SDK-shipped)

```
metadata/
├── e1m_modules/
│   ├── E1M-AEN301.yaml      # partial_hw_config: true (silicon-only fields filled; SKU memory + per-SKU TBDs)
│   ├── E1M-AEN401.yaml      # partial_hw_config: true
│   ├── E1M-AEN501.yaml      # partial_hw_config: true
│   ├── E1M-AEN601.yaml      # partial_hw_config: true
│   ├── E1M-AEN701.yaml      # lower-priority E7 preset
│   ├── E1M-AEN801.yaml      # lead AEN E8 preset
│   ├── E1M-V2N101.yaml      # v0.3 fully-populated worked example
│   ├── E1M-V2N102.yaml      # partial_hw_config: true
│   ├── E1M-V2M101.yaml      # V2N-M1 SKU (DEEPX-DXM1 populated)
│   ├── E1M-V2M102.yaml      # V2N-M1 SKU
│   └── E1M-NX9101.yaml      # i.MX 93 placeholder MPN (production E1M-NX9xxx TBD)
└── boards/
    ├── e1m-evk.yaml            # 35x35 EVK (AEN / N93)
    ├── e1m-x-evk.yaml          # 45x65 EVK (V2N / V2N-M1)
    └── custom-example.yaml     # template downstream consumers copy + edit
```

v0.3 ships the schema + ten production SoM presets, the
placeholder N93 bring-up preset (`E1M-NX9101`), the two stock
boards, and a copy-friendly custom-example template.  Two SKUs
(`E1M-AEN801`, `E1M-V2N101`) are the primary worked presets; lower-priority
or not-yet-final SKUs carry `partial_hw_config: true` so
the loader knows to expect SKU-specific overrides from the
consumer's `board.yaml`.  Per the project memory note, values
not in the silicon datasheet stay `TBD` (e.g.
`board_id.adc_channel` in family-level `hw-revisions.yaml`)
until the user supplies them authoritatively.

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

### `extra_libraries` -- open-set escape hatch (v0.6)

The curated `libraries:` set is closed -- it's the set of manifests
under `metadata/libraries/`.  For one-off vendor SDKs, research-only
deps, or libraries on their way into the curated set, the per-core
`extra_libraries:` provides an open-set escape hatch.  Each entry
declares either an inline Kconfig fragment OR a `profile:` path
(mutually exclusive, enforced by the loader):

```yaml
libraries:
  - name: mbedtls         # curated -- resolves to a manifest
    cores: [m55_hp]

cores:
  m55_hp:
    app: ./src
    extra_libraries:
      - name: zforce      # one-off vendor SDK, inline Kconfig path
        include_path: third_party/zforce/include
        kconfig:
          - CONFIG_ZFORCE=y
          - CONFIG_ZFORCE_I2C_ADDR=0x50
      - name: mycrypto    # library with per-silicon backend selection
        profile: third_party/mycrypto/hw-backends.yaml
```

The two shapes:

| Field      | When to use                                                                                              |
|------------|----------------------------------------------------------------------------------------------------------|
| `kconfig:` | Fast path for one-off libraries.  Lines emit verbatim into the slice's `alp.conf`.                       |
| `profile:` | Library wants per-silicon backend selection consistent with the curated `libraries:`.  See below.        |

The `profile:` file follows the same shape as a curated library's
folded `integration.zephyr.hw_backends` block: an `accelerators:`
list of priority entries (each with optional `silicon:` /
`soc_family:` / `requires_cap:` matchers + a `kconfig:` directive)
and an optional `sw_fallback:` block whose `kconfig:` always emits.
First match per `class:` wins; see any
[`metadata/libraries/<name>.yaml`](../metadata/libraries/) manifest
for a worked example.

Loader rules (enforced by `_validate_consistency()` in
`scripts/alp_orchestrate/`):

- Each entry MUST declare exactly one of `kconfig:` / `profile:`.
- `name:` is globally unique across every core's
  `extra_libraries:`.
- `name:` must NOT collide with a curated `libraries:` name --
  use the curated path for curated entries.
- `profile:` must resolve to a file (repo-relative).

### `libraries` (ADR 0018) {#libraries-project-wide-adr-0018}

The **top-level** `libraries:` key (a sibling of `som:` / `cores:`,
not nested under a core) is the single place curated third-party
libraries are declared — GUI, DSP/NN, serialization, and so on.  Each
entry is a `{name, cores?}` object: omit `cores:` for a project-wide
selection, or list core ids to scope the library to specific slices.
A bare name is shorthand for a project-wide `{name}`:

```yaml
som:
  sku: E1M-AEN801
libraries:
  - name: lvgl                 # project-wide (every core the manifest supports)
  - name: nanopb
  - name: cmsis-dsp
    cores: [m55_hp]            # scoped to one core
cores:
  m55_hp:
    app: ./src
```

Each name resolves to a manifest at
[`metadata/libraries/<name>.yaml`](../metadata/libraries/) — the single
source of truth for that library's per-OS wiring, pinned upstream
version, SPDX licence, curation tier, and compatibility constraints.
The orchestrator emits the wiring through the ordinary `--emit`
contract (ADR 0014): the library's Kconfig symbols land in each Zephyr
slice's `alp.conf`, its `IMAGE_INSTALL` entries in each Yocto slice's
`local.conf`, its CMake pin in each baremetal slice's args. Selecting a
library the target cannot satisfy fails emit with the failing
constraint named — the same clear-error contract as schema validation.

Two curation tiers bound CI cost (ADR 0018):

- **Tier A — curated**: version-pinned, built in alp-sdk CI for at
  least one board per family, ships a teaching example. Breakage blocks
  release.
- **Tier B — recipe-only**: wiring + compatibility metadata are
  maintained and emitted, but the library is not built in alp-sdk CI.
  `alp doctor` labels it.

`alp doctor` reports the selected libraries for the project in scope
(tier + licence + compatibility), reading the same manifests, so the
CLI and alp-studio's library picker never disagree.

Scoping a library to specific cores (`cores: [<id>]`) folds in what
earlier schema drafts spelled as a separate per-core
`cores.<id>.libraries:` token list — there is now one library
declaration, manifest-driven, and the per-core list is gone.  The
compile-time config headers under `metadata/library-profiles/<lib>/`
(e.g. `etl_profile.h`, `lv_conf.h`) still tune each library for the
SDK's invariants; the HW-backend accelerator model is folded into each
`metadata/libraries/<name>.yaml` manifest.

See [`metadata/libraries/README.md`](../metadata/libraries/)
for the manifest shape, the full library list, and how to add one.

#### Flagship: micro-ROS + ROS 2 across one heterogeneous project

The `libraries:` mechanism spans both OSes of a heterogeneous SoM in a
single project — the proof ADR 0018 exists for and the peer model
[ADR 0010](adr/0010-heterogeneous-os-orchestration.md) defines. A
robotics project runs a **micro-ROS** node on the Cortex-M / Zephyr
peer and **ROS 2** on the Cortex-A / Yocto peer, both selected from the
same top-level `libraries:` key:

```yaml
som:
  sku: E1M-V2N101
libraries: [micro-ros, ros2]   # M-side client + A-side agent, one file
cores:
  a55_cluster:                 # Cortex-A55 -> Yocto runs ROS 2
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:                      # Cortex-M33 -> Zephyr runs the micro-ROS node
    os: zephyr
    app: ./m33
```

Each library resolves to the peer it belongs on: `micro-ros`
(`requires: {os: [zephyr], core_class: m}`) wires only into the Zephyr
slice's `alp.conf`; `ros2` (`requires: {os: [yocto], core_class: a}`)
appends `rclcpp` to the Yocto slice's `IMAGE_INSTALL`. Select either on
the wrong peer and emit fails naming the `os` / `core_class` constraint.

Two honest limits are recorded in the manifest headers rather than
hidden:

- **micro-ROS is not yet pinned** in the Zephyr v4.4.0 `west.yml`. Its
  manifest names the upstream `micro_ros_zephyr_module` (branch
  `humble`) as a west prerequisite and enables **by module presence**
  (no invented Kconfig); emit renders the selection tag with no
  `CONFIG_` line until the module is added to `west.yml`.
- **ROS 2 is Tier B (recipe-only)**: its wiring is grounded in
  `meta-alp-sdk` (`rclcpp`; `meta-ros2-humble` as a `LAYERRECOMMENDS`),
  but alp-sdk CI does not build it, and a build must add
  `meta-ros2-humble` to `bblayers.conf`.
- The **cross-core RMW bridge** that carries ROS topics between the two
  peers (UDP-on-virtio, or a custom RMW over the RPMsg transport of
  [ADR 0016](adr/0016-cross-core-peripheral-proxy-wire-schema.md)) is
  bench-gated and out of scope for the manifests — it is its own
  design.

