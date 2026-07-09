# Project configuration (`board.yaml`)

`board.yaml` is the **single file** that declares what a firmware
project targets: which SoM SKU (MPN), which board (daughter
board), which app + libraries + peripherals per core, which
optional connectivity features.  Consumers place one at their app
root, fill in their MPN, and the SDK's loader handles the rest.

Silicon-determined facts (which NPUs the SoM ships with, on-module
memory + components, the natural OS for each core class) are
dictated by the SoM SKU preset under `metadata/e1m_modules/<MPN>.yaml`
and are NOT customer-facing knobs.

## Quick start: minimum-viable `board.yaml`

Paste this into `board.yaml` at your app root, change the `sku`
to your MPN, and you're done -- everything else is inherited from
the SDK's per-MPN preset:

```yaml
som:
  sku: E1M-AEN801        # your MPN -- the SDK ships a preset
                          # at metadata/e1m_modules/<MPN>.yaml

preset: e1m-evk          # or write your custom board out inline -- see below

cores:
  m55_hp:                # core IDs come from the SoC spec under
    os: zephyr           # metadata/socs/<vendor>/<family>/<part>.json
    app: ./src           # path (relative to board.yaml) of the Zephyr app
```

That's the whole config for a vanilla "E1M-AEN801 on the EVK,
M55-HP core running Zephyr" build.  Optional per-core blocks
(`peripherals`, `libraries`, `inference`, `iot`, `diagnostics`)
add capability on top — omit them to get the defaults from the
MPN's SoM preset (`metadata/e1m_modules/<MPN>.yaml` `topology:`
block).  See
[`docs/heterogeneous-builds.md`](heterogeneous-builds.md) for the
multi-core (`a55_cluster` + `m33_sm`, `a32_cluster` + `m55_hp` +
`m55_he`) shape and the cross-core `ipc:` block.

SDK-shipped SoM presets (look under
`metadata/e1m_modules/<MPN>.yaml`):

| Family            | MPNs (paste any into `som.sku`)                                              |
|-------------------|------------------------------------------------------------------------------|
| Alif Ensemble     | `E1M-AEN301`, `AEN401`, `AEN501`, `AEN601`, `AEN701`, `AEN801`               |
| Renesas RZ/V2N    | `E1M-V2N101`, `V2N102`                                                       |
| RZ/V2N + DEEPX    | `E1M-V2M101`, `V2M102`                                                       |
| NXP i.MX 93       | `E1M-NX9101` (placeholder MPN; production `E1M-NX9xxx` TBD pending HW config) |

Stock board presets (paste into `preset:`):

| Preset       | Form factor | Hosts                                  |
|--------------|-------------|----------------------------------------|
| `e1m-evk`    | 35×35       | E1M-AEN family, future E1M-N93 family  |
| `e1m-x-evk`  | 45×65       | E1M-X V2N family, V2N-M1 family        |

`preset:` is the SDK-internal shortcut the demos use.  For a
**custom board** (your own PCB design), drop the `preset:` line
and write the board out inline at the top level (`name:`,
`populated:`, `e1m_routes:`) -- see the "Board declaration"
section below.

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
  `prj.conf` in a v0.3 alp-sdk app is empty (or carries only a
  comment).  The application's `CMakeLists.txt` invokes
  `scripts/alp_project.py` at configure time and layers the
  generated `alp.conf` over `prj.conf` via Zephyr's
  `OVERLAY_CONFIG` cmake variable.  `rsource` is NOT valid in a
  `.conf` file (it is a Kconfig-source directive only) -- the
  earlier "single `rsource` line" pattern in this doc was wrong;
  see the worked example at `examples/peripheral-io/gpio-button-led/CMakeLists.txt`
  for the correct wiring.
- **Don't pass extra `-D` flags to `cmake` for SDK options.**  The
  loader emits the right set; passing extra flags risks divergence
  from the declared config.
- **Don't hand-edit `local.conf`'s `MACHINE` / `IMAGE_INSTALL`.**
  Generate them via `--emit yocto-conf` and require the result.

If you find yourself reaching for a hand-edit because `board.yaml`
can't express what you want, file an issue -- the right fix is to
extend the schema, not to bypass it.

### Today's gaps (v0.3 -> v0.4)

`board.yaml` covers the SoM + board + OS backend + inference + IoT
features + optional libraries + Zephyr peripherals today.  One
remaining gap where hand-written config still leaks in, targeted
for v0.4:

1. **Per-test `prj.conf` in `tests/zephyr/<area>/`.**  The
   in-repo test infrastructure still uses hand-written
   `prj.conf` files.  These are SDK-internal (not consumer-
   facing) and stay as-is until the loader handles test-style
   configs in v0.4.

DTS overlays for board wiring (`--emit dts-overlay`) and
`west.yml` libraries auto-pinning (`--emit west-libraries`) were
previously v0.4 gaps; both ship in v0.3.  The loader's `--emit dts-overlay` mode
parses `include/alp/boards/<board>.h` and generates the bus
aliases (`alp-i2c<N>`, `alp-spi<N>`, `alp-uart<N>`, `alp-pwm<N>`)
plus a POSITIONAL `alp,pin-array` whose entry N is E1M pad N in the
canonical `e1m_pinout.h` order (52 slots: IO0..25 = 0..25, PWM0..7 =
26..33, ENC0_X..ENC3_Y = 34..41, ADC0..7 = 42..49, DAC0..1 = 50..51).
Per-pad GPIO bank/index columns remain TBD until the
upstream SoM board files land in `alplabai/alp-zephyr-modules`;
the customer fills those in place without renumbering.

For v0.3, consumers writing apps from scratch should still use
`board.yaml` as the canonical config and treat the per-test
`prj.conf` gap as a short-term hand-override.  The migration path
to the v0.4 single-source-of-truth model is purely additive on
top of v0.3's schema.

## Why one file

Pre-`board.yaml` the user had to track configuration across:

| Path | What it picked |
|------|-----------------|
| `prj.conf` (Zephyr) | `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y` + Kconfig knobs |
| cmake configure args | `-DALP_OS=baremetal -DALP_SOM=aen` |
| `local.conf` (Yocto) | `MACHINE=e1m-v2m101-a55` + IMAGE_INSTALL |
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
| `libraries`      | no       | Project-wide curated third-party libraries (ADR 0018), e.g. `[lvgl, cmsis-dsp]`.  Each names a manifest under `metadata/libraries/<name>.yaml`; the orchestrator emits its per-OS wiring and rejects an incompatible selection at emit time.  See [`libraries` (project-wide, ADR 0018)](#libraries-project-wide-adr-0018) below.  Distinct from the per-core `cores.<id>.libraries:` token list. |
| `diagnostics`    | no       | `alp_last_error()` + log level.                               |

*Either `preset:` (preset mode) or inline `name:` + `populated:` +
`e1m_routes:` (customer path).  Both omitted is also fine -- a
headless / inference-only build with no board declaration.

Per-core fields under `cores.<id>` (all optional, all inherit from
the SoM preset's `topology.<id>` when omitted):

| Field          | Notes                                                                                  |
|----------------|----------------------------------------------------------------------------------------|
| `app`          | App source dir.  Required for `os: zephyr` / `os: baremetal`.                          |
| `image`        | Yocto image recipe name (e.g. `alp-image-edge`).                                       |
| `os`           | NOT an OS picker — the runtime is class-derived (M→Zephyr, A→Yocto). Only `off` (skip slice) or `baremetal` (rare) are settable; a cross-class OS is rejected. |
| `peripherals`  | Zephyr subsystem / Yocto package list for this slice.                                  |
| `libraries`    | Library opt-in list for this slice.                                                    |
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
                            # table (see "Hardware revision tracking"
                            # below); at runtime the SDK reads the
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

`libraries:` is closed -- the schema's enum lists the 25 libraries
the SDK curates.  For one-off vendor SDKs, research-only deps, or
libraries on their way into the curated set, `extra_libraries:`
provides an open-set escape hatch.  Each entry declares either an
inline Kconfig fragment OR a `profile:` path (mutually exclusive,
enforced by the loader):

```yaml
cores:
  m55_hp:
    app: ./src
    libraries:
      - mbedtls           # curated -- closed enum
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

The `profile:` file follows the same shape as
`metadata/library-profiles/<lib>/hw-backends.yaml`: an
`accelerators:` block of priority entries (each with optional
`silicon:` / `soc_family:` / `requires_cap:` matchers + a
`kconfig:` directive) and an optional `sw_fallback:` block whose
`kconfig:` always emits.  First match per `class:` wins; see the
[`mbedtls` profile](../metadata/library-profiles/mbedtls/hw-backends.yaml)
for a worked example.

Loader rules (enforced by `_validate_consistency()` in
`scripts/alp_orchestrate/`):

- Each entry MUST declare exactly one of `kconfig:` / `profile:`.
- `name:` is globally unique across every core's
  `extra_libraries:`.
- `name:` must NOT collide with the curated `libraries:` enum --
  use the curated path for curated entries.
- `profile:` must resolve to a file (repo-relative).

### `libraries` (project-wide, ADR 0018) {#libraries-project-wide-adr-0018}

The **top-level** `libraries:` key (a sibling of `som:` / `cores:`,
not nested under a core) selects *curated third-party libraries* the
SDK integrates across the whole project — GUI, DSP/NN, serialization,
and so on:

```yaml
som:
  sku: E1M-AEN801
libraries: [lvgl, cmsis-dsp, nanopb]   # <-- project-wide
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

**This is a different mechanism from the per-core `cores.<id>.libraries:`
token list above.** The per-core list is a closed enum wired through
`metadata/library-profiles/` (compile-time config headers + HW-backend
bindings); the top-level `libraries:` is the manifest-driven ADR 0018
selection. Use the top-level key for the curated third-party libraries
that ship a `metadata/libraries/<name>.yaml` manifest.

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

## How the loader compiles the file

`scripts/alp_project.py` reads `board.yaml`, validates against the
schema, resolves the SoM SKU + board presets, applies overrides,
and emits the requested build artifacts.  Common workflows below.

### West extension -- validate, generate, and build

The SDK ships first-class west extension commands via
[`scripts/west-commands.yml`](../scripts/west-commands.yml).  The
usual application build entry point is:

```bash
west alp-build -b <board> <app-dir>
```

`west alp-build` validates `<app-dir>/board.yaml`, emits the generated
per-slice configuration and system manifest, then dispatches the
underlying Zephyr / Yocto / baremetal build steps for the enabled
cores.  Companion commands (`west alp-image`, `west alp-flash`,
`west alp-clean`, `west alp-emit`, `west alp-size`, and
`west alp-renode`) consume the same build state for bundle, flash,
inspection, sizing, and simulation workflows.

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

The application's `CMakeLists.txt` wires the loader as a
configure-time step and layers the result over `prj.conf` with
`OVERLAY_CONFIG`.  `prj.conf` itself stays empty -- everything
flows from `board.yaml`:

```cmake
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Point ALP_SDK_ROOT at your alp-sdk checkout (env override, else a
# tree-relative fallback -- mirrors the examples/*/CMakeLists.txt pattern).
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
endif()

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rv=${_alp_rv})")
endif()
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app LANGUAGES C)
target_sources(app PRIVATE src/main.c)
```

Zephyr's `OVERLAY_CONFIG` machinery merges the generated `alp.conf`
on top of `prj.conf` at Kconfig time -- the app picks up every
`CONFIG_*` line the loader emitted.

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

### west.yml libraries auto-pin (`--emit west-libraries`)

`--emit west-libraries` produces a `west.yml` fragment listing the
Zephyr modules and exact west project pins the board.yaml's library
declarations require.  Zephyr-owned modules (`lvgl`, `mbedtls`,
`cmsis-dsp`, `fs/littlefs`) land in the `zephyr` import
`name-allowlist:`.  ADR 0018 libraries whose manifests carry an
`integration.zephyr.west` pin, such as `aws-iot` and `azure-iot`,
land as concrete `projects:` entries with exact upstream release
tags.  Header-only C++ libraries (`etl`, `fmt`, `nlohmann_json`,
`doctest`) aren't Zephyr modules -- they ride the loader's
compile-time profile hook instead -- so the emitter lists them in a
trailing comment rather than the allowlist.

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit west-libraries \
    --output build/generated/alp-west-libs.yml
```

Import the fragment from your application's `west.yml`:

```yaml
manifest:
  projects:
    - name: alp-app-libs
      path: alp-app-libs
      import: build/generated/alp-west-libs.yml
```

Run `west update` and only the modules board.yaml actually
references land in the workspace.  Closes the second v0.4 gap
this doc previously flagged.

### Build-time identifier header (`--emit hw-info-h`)

`--emit hw-info-h` produces an auto-generated companion header to
[`<alp/hw_info.h>`](../include/alp/hw_info.h) that bakes the
customer's board.yaml identifiers into `ALP_HW_BUILD_*` string
macros.  Apps include both headers and pass the compile-time
constants to the runtime check:

```c
#include "alp/hw_info.h"
#include "alp_hw_info_build.h"   /* generated */

alp_hw_info_t info;
alp_hw_info_read(&info);
alp_hw_info_assert_matches_build(&info,
                                 ALP_HW_BUILD_SOM_SKU,
                                 ALP_HW_BUILD_SOM_HW_REV);
```

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit hw-info-h \
    --output build/generated/alp_hw_info_build.h
```

Macros emitted: `ALP_HW_BUILD_SOM_SKU`, `ALP_HW_BUILD_SOM_FAMILY`,
`ALP_HW_BUILD_SOM_HW_REV`, `ALP_HW_BUILD_OS`, and (when a board
is declared) `ALP_HW_BUILD_BOARD_NAME` +
`ALP_HW_BUILD_BOARD_HW_REV`.

### DTS overlay for board wiring (v0.3)

`--emit dts-overlay` reads the board header at
`include/alp/boards/<board>.h`, finds every `#define X
E1M_<class><N>` macro for the v0.3-scoped classes (I2C, SPI,
UART, PWM, GPIO_IO), and emits a Zephyr `.overlay` declaring:

- One alias per bus channel the board wires (`alp-i2c<N> =
  &i2c<N>;`, `alp-spi<N> = &spi<N>;`, `alp-uart<N> = &uart<N>;`,
  `alp-pwm<N> = &pwm<N>;`).  The trailing comment on each alias
  lists every macro in the board header that references that
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

The dts-overlay emitter still synthesizes only the bus aliases and
GPIO pin array above.  Display devices are supported through the
Zephyr devicetree path today, but the board preset or app overlay
must provide the concrete display node.  For SPI TFTs, use Zephyr's
MIPI DBI Type C binding (`compatible = "zephyr,mipi-dbi-spi"`) with
the panel driver child (for example `sitronix,st7789v`), then expose
that child as `zephyr,display` for LVGL and/or `alp-display0` for
`<alp/display.h>`.  Application code stays on LVGL or the portable
display API; it does not initialise the panel driver directly.

First-class display synthesis in the dts-overlay emitter remains a
follow-up once the upstream SoM board files lock the gpio bank/index
columns.  Encoders, cameras, and other non-bus device classes follow
the same rule.

### What the loader does NOT yet do

- **Cross-validation against `metadata/socs/*.json`** -- the loader
  trusts the SKU preset's `silicon:` field; it doesn't yet
  validate that requested features (e.g. 16-bit ADC) match the
  SoC's documented caps.  The `<alp/soc_caps.h>` runtime check
  catches mismatches at `_open` time today.

## Hardware revision tracking

Every released SoM family and every released board carries a
`hw_revisions:` table.  The SDK uses it to detect "wrong firmware
for this hardware" two ways:

- **Build-time** -- the loader + validator read
  [`metadata/sdk_version.yaml`](../metadata/sdk_version.yaml) and
  fail-fast if the chosen `hw_rev`'s
  `[min_sdk_version, max_sdk_version]` window doesn't cover the
  current SDK version.  Validator exit code `3`; loader aborts the
  CMake configure with a clear error.
- **Runtime** -- the SDK boots into a board-ID check that uses
  a single ADC pin per board (SoM-side and board-side) fed by a
  resistor divider from a 1.8 V rail.  Each `hw_rev` entry's
  `board_id:` sub-block fixes the divider resistor values and the
  nominal mV reading the SDK looks for.  A second, finer-grained
  check reads the SoM's on-module 24C128 EEPROM (the AEN family
  populates one by default) for an authoritative MPN string +
  serial + mfg date -- the production-test flow writes the
  manifest, the SDK matches it against `board.yaml`'s
  `som.sku`.  Mismatch on either tier halts boot.

### Why one ADC pin (instead of GPIO straps)

The E1M form factor has no spare GPIO pads for board-ID resistor
straps -- every pad is allocated by the spec.  A single ADC
channel with a resistor divider distinguishes up to ~8 revisions
at +/-100 mV bin radius (with 1 % resistors on a 1.8 V rail);
that is enough for many family respins and leaves the rest of the
GPIOs free for the application.  Per-rev resistor + voltage
choices are documented in each family file's `board_id:` block;
the canonical math lives at
[`metadata/e1m_modules/aen/hw-revisions.yaml`](../metadata/e1m_modules/aen/hw-revisions.yaml).

### How the data is laid out

```
metadata/
├── sdk_version.yaml                            # SDK release version (currently "version: 0.9.0")
├── e1m_modules/
│   ├── aen/hw-revisions.yaml                   # family-level revs (AEN family
│   │                                            #  shares one PCB; SKUs differ
│   │                                            #  by silicon only).
│   ├── v2n/hw-revisions.yaml                   # V2N family revs (board_id.adc_channel TBD)
│   ├── v2n-m1/hw-revisions.yaml                # V2N-M1 family revs (mirrors V2N + DEEPX)
│   ├── imx93/hw-revisions.yaml                 # i.MX 93 family revs (adc_channel TBD)
│   └── E1M-AEN801.yaml                     # MPN preset; `default_hw_rev: r2`
│                                                #  points into the family table.
└── boards/
    ├── e1m-evk.yaml                            # board preset; carries its own
    │                                            #  hw_revisions + default_hw_rev.
    ├── e1m-x-evk.yaml                          # V2N / V2N-M1 board
    │                                            #  (board_id.adc_channel TBD).
    └── custom-example.yaml                     # copy-friendly template
```

`board.yaml` overrides go in the `som.hw_rev` / `board.hw_rev`
fields described above.  Omit them on stock builds -- the preset's
`default_hw_rev` is picked up automatically.

## Modular SoM: optional chip populations

The SoM YAML carries a per-chip **`assembled:`** flag for every entry
in its `i2c_devices:` topology so the SDK can express SoMs that ship
in multiple BOM variants (same SKU, different chip populations).
Three states:

| `assembled:`  | Meaning                                                                |
|---------------|------------------------------------------------------------------------|
| `true` *(default)* | Chip is always populated on every BOM variant of this SKU.       |
| `false`            | DNI (Do Not Install) -- the chip footprint exists but is empty.  |
| `"optional"`       | Per-BOM-variant -- some units have it, some don't.               |

Example (extract from `metadata/e1m_modules/E1M-V2N101.yaml`):

```yaml
i2c_devices:
  brd_i2c:
    devices:
      - { chip: rv3028c7,  role: rtc,           address_7bit: "0x52" }
      - { chip: act8760,   role: pmic_main_p0,  address_7bit: "0x25" }
      - { chip: tps628640, role: lpddr4x_0v6,   address_7bit: "0x4D",
          assembled: optional }      # only some BOM variants
```

The lint at `scripts/check_example_portability.py` reads this flag
and prints `NOTE` lines whenever an example's `chips:` list reaches
for a `assembled: optional` chip on its target SKU.  Customer code
that uses an optional chip MUST handle `alp_*_init` returning
`ALP_ERR_NOT_READY` gracefully (skip the demo, log a clear message,
fall back to a different code path) instead of crashing.

### Runtime: detecting which chips are populated

Two complementary mechanisms:

1. **`<alp/hw_info.h>`**.  `alp_hw_info_read()` reads the SoM
   manifest from the on-module 24C128 EEPROM (`metadata/templates/
   eeprom_manifest.yaml` for the layout).  Production-test wrote
   the manifest; firmware checks `hw_info.som_hw_rev` +
   capability flags to know which chip set this unit shipped with.
2. **Probe-and-fall-back**.  Every chip driver's `_init()`
   ACK-probes the I2C bus.  If the chip isn't populated the
   driver returns `ALP_ERR_NOT_READY`; firmware branches off
   that.  This is the right mechanism for runtime discovery on
   boards that don't carry the SoM manifest.

The two mechanisms cooperate: `<alp/hw_info.h>` answers "what was
this unit *intended* to carry?", and the per-chip `_init()` probe
answers "is the chip *actually* responding right now?".  When they
agree, the firmware proceeds; when they disagree, that's a
production-test follow-up signal (likely a mis-strap or assembly
defect).

### When you'd add a new optional flag

If your board strips a chip the upstream preset declares populated,
the right approach is **per-app override in `board.yaml`** rather
than editing the preset:

```yaml
# my-app/board.yaml
som:
  sku: E1M-V2N101
  overrides:
    on_module:
      i2c_devices:
        brd_i2c:
          devices:
            - { chip: optiga_trust_m, assembled: false }
            # other devices inherit from the preset
```

The loader merges your overrides onto the preset before generating
the build config.  No SDK fork needed.

## Build-system integration knobs

For declarative coverage of build options that customers used to
hand-edit in `prj.conf` / `local.conf` / sysbuild.conf,
`board.yaml` carries a small set of structured blocks.  All
optional; sensible defaults from the SoM family / SDK baseline
apply when omitted.

### Hardware-info EEPROM (`features.hw_info.eeprom:`)

```yaml
features:
  hw_info:
    eeprom:
      bus: e1m_i2c0        # -> CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID=0
      addr_7bit: 0x50      # CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT
      offset: 0            # CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET
```

Project-wide.  This declares where the 128-byte
`alp_hw_info_eeprom_t` manifest lives when an app needs to pin the
EEPROM bus/address/offset explicitly.  The orchestrator emits the
values into Zephyr `alp.conf` and projects them into
`system-manifest.yaml` under `hw_info.eeprom`.

`features:` is not a free-form pass-through.  Only typed feature
blocks with emitter support are accepted; unsupported keys are schema
errors so configuration cannot be silently ignored.

### Per-slice memory (`cores.<id>.memory:`)

```yaml
cores:
  m55_hp:
    app: ./src
    memory:
      stack_kib:     8       # CONFIG_MAIN_STACK_SIZE  = 8192
      heap_kib:      4       # CONFIG_HEAP_MEM_POOL_SIZE = 4096
      isr_stack_kib: 2       # CONFIG_ISR_STACK_SIZE  = 2048
```

Zephyr slice only; ignored on Yocto / baremetal.  Replaces
hand-edited `CONFIG_MAIN_STACK_SIZE` etc. in `prj.conf`.

### Per-slice power (`cores.<id>.power:`)

```yaml
cores:
  m55_he:
    app: ./src
    power:
      sleep_mode:     standby      # disabled | idle | standby | deep
      wakeup_sources: [uart, gpio, rtc]
```

`sleep_mode != disabled` emits `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y`
and lands the per-state hierarchy.  `wakeup_sources:` entries that
name a subsystem (`uart`, `gpio`, ...) emit
`CONFIG_PM_DEVICE_WAKE_<SUBSYS>=y`; `E1M_*` pad names emit a hint
comment (per-silicon wake-pin Kconfig lands in v0.7).

### Per-module log levels (`diagnostics.modules:`)

```yaml
diagnostics:
  log_level: info               # default for everything else
  modules:
    alp_iot:       debug
    alp_inference: warn
    alp_security:  "off"        # quote 'off' -- bare off parses as YAML false
```

Emits `CONFIG_<MODULE>_LOG_LEVEL=<n>` per entry into the slice's
`alp.conf`.  Saves customers from finding the right
`CONFIG_LOG_*` symbol per module.

### Bootloader (`boot:` -- MCUboot)

```yaml
boot:
  method: mcuboot                # mcuboot | none
  signing:
    algorithm: ecdsa_p256        # ecdsa_p256 | rsa2048 | rsa3072 | ed25519
    key_file: keys/prod_ecdsa_p256.pub.pem
  slots:
    primary:   { size_kib: 480 }
    secondary: { size_kib: 480 }
  swap_algorithm: scratch        # scratch | move | overwrite
  scratch_size_kib: 32
  anti_rollback: false
```

Project-wide (one bootloader per device).  The loader emits a
sysbuild.conf overlay with the corresponding `SB_CONFIG_*` lines
(MCUboot signature type, slot sizes, swap algorithm, scratch size,
anti-rollback counter).  See `docs/secure-boot.md` for the
underlying secure-boot contract.

### OTA (`ota:` -- Mender / MCUmgr)

```yaml
ota:
  provider: mender               # mender | mcumgr | none
  artifact_name: my-product-1.2.3
  signing_key: keys/mender_artifact.pem
  server:
    url:    https://hosted.mender.io
    tenant: my-tenant
  rollback: { enabled: true, retries: 3, min_version: 1 }
  poll_interval_s: 1800
  storage:
    device: /dev/mmcblk0p        # Yocto-only
    boot_part_mb: 64
    rootfs_ab: true
    total_size_mb: 4096
```

Project-wide, provider-driven dispatch (ADR 0009 resolved):

| Provider   | Yocto emit                              | Zephyr emit                                                                       |
|------------|-----------------------------------------|-----------------------------------------------------------------------------------|
| `mender`   | `MENDER_*` weak-assigns in `local.conf` | `CONFIG_MENDER_MCU_CLIENT=y` + URL/tenant/poll Kconfig (out-of-tree, west.yml)    |
| `hawkbit`  | n/a (Zephyr only)                       | `CONFIG_HAWKBIT=y` + `HAWKBIT_SERVER` + `HAWKBIT_POLL_INTERVAL` (Zephyr upstream) |
| `mcumgr`   | n/a (Zephyr only)                       | `CONFIG_MCUMGR=y` + GRP_IMG/GRP_OS (transport is the app's call)                  |
| `none`     | OTA disabled                            | OTA disabled                                                                      |

The validator (P2.3 rule 1) requires:
- `provider: mender` → at least one `cores.<id>.os: yocto` OR `cores.<id>.os: zephyr`
- `provider: hawkbit` → at least one `cores.<id>.os: zephyr`
- `provider: mcumgr` → at least one `cores.<id>.os: zephyr`

When `provider: mender` and at least one Zephyr slice is present,
the west-libraries emitter (`--emit west-libraries`) auto-adds a
`mender-mcu-client` entry to the `name-allowlist:` so the customer's
`west update` pulls the module without hand-edits.  Hawkbit and
MCUmgr are upstream Zephyr modules so they don't need a west.yml
entry.

See `docs/ota.md` + `docs/ota-device-contract.md` + ADR 0009.

### Storage partitions (`storage:`)

```yaml
storage:
  - { name: settings,        fs: littlefs, size_kib: 64,  mount: /lfs/settings,
      flash_device: mram_main }
  - { name: app_data,        fs: littlefs, size_kib: 128, mount: /lfs/app,
      flash_device: mram_main }
  - { name: mcuboot_scratch, fs: raw,      size_kib: 32,
      flash_device: mram_main }
  - { name: pinned_low,      fs: raw,      size_kib: 32,
      flash_device: mram_main, offset_kib: 0 }    # explicit offset override
```

Project-wide.  Each entry declares a fixed partition on the
referenced flash device.  Partitions on the same device are
**name-sorted** and allocated **bottom-up, page-aligned to 4 KiB**
so layouts stay byte-stable across rebuilds (the address determinism
property OTA images depend on).  Supply `offset_kib:` to pin an
entry to an explicit offset within the device -- useful for
coexistence with bootloader-managed slots or when migrating a
legacy layout.

`flash_device:` resolves against the SoM preset in this order:

1. `memory_map:` region names (e.g. `mram_main`, `ocram_low`) --
   either declared on the SoM or auto-derived from the SoC variant's
   `mram_mb` / `sram_banks_kb` (the same resolution `resolve_memory_map()`
   does for IPC carve-outs).
2. `on_module.ospi_memories:` keys (e.g. `ospi0`) -- external OSPI
   flash declared on the SoM.

The loader rejects typoed `flash_device:` references at parse time
with the list of known devices for the project's SoM.  When the
referenced device's size is `TBD` (HW-config still owed), the
resolver projects the entry as `status: blocked` in the generated
manifest with a reason that points at the SoM file owing the value.

The orchestrator emits two artefacts per project:

* `dts-partitions.dtsi` -- a DTS overlay that decorates each
  referenced flash device with a `partitions { compatible =
  "fixed-partitions"; ... };` child node carrying every resolved
  partition's `label`, `reg`, and DT phandle (`<name>_partition`).
  Materialised under `build/generated/` alongside
  `dts-reservations.dtsi` (IPC).
* Per-fs Kconfig in each Zephyr slice's `alp.conf`:
  `CONFIG_FILE_SYSTEM=y`, plus `CONFIG_FILE_SYSTEM_LITTLEFS=y` /
  `CONFIG_FAT_FILESYSTEM_ELM=y` / `CONFIG_FILE_SYSTEM_EXT2=y`
  per the `fs:` enum, plus `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y`
  per littlefs entry.

An optional static C mount table (`--emit storage-mounts-c`)
generates a `fs_mount_t` per entry with a `mount:` declared, plus an
aggregate `alp_storage_mounts[]` array for boot-time iteration.

Inspect the resolved layout with:

```bash
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit dts-partitions
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit system-manifest \
    | yq '.storage[]'
```

The `system-manifest.yaml` carries every resolved partition's
`offset_kib`, `size_kib`, `dt_label`, `mount`, and (when blocked)
`reason:` so reviewers see the full flash map alongside IPC carve-outs.

### PSA Crypto + TF-M (`security.psa:`)

```yaml
security:
  psa:
    persistent_slots:  16
    its_storage:       mram_main          # SoM memory_map region OR storage[] name
    ps_storage:        ospi0              # optional (PS)
    tfm:               true               # enable TF-M secure partition
    attestation_root:  optiga_trust_m     # optiga_trust_m | tfm_internal | none
```

Project-wide.  When `tfm: true` the orchestrator emits a sysbuild
child-image overlay at `build/sysbuild/tfm/tfm.conf` containing
`SB_CONFIG_TFM=y`, `SB_CONFIG_TFM_BUILD_TYPE=<Release|Debug|MinSizeRel>`
(inherited from `boot.build_type` when set), the
`CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT` value, and string-form
`CONFIG_PSA_CRYPTO_{ITS,PS}_BACKING_STORE` references resolving to a
`storage[].name` or a SoM `memory_map:` region.  When `tfm: false`
PSA Crypto runs entirely non-secure (mbedTLS-only) and the overlay is
not written.

`attestation_root: optiga_trust_m` only validates when the SoM preset
physically ships OPTIGA Trust M (AEN family + V2N family today); the
emitter additionally surfaces `CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y`
and a comment pointing at the `src/security/optiga_trust_m_bridge.c`
PSA <-> OPTIGA bridge driver.

The TF-M secure partition runs on the same M55-HP core as the
non-secure app via the Armv8-M security extension (TrustZone-M split,
not a separate M55-HE core).  See `docs/adr/0013-tfm-boundary-m55-hp-trustzone.md`
+ `docs/security-audit-plan.md` + ADR 0010.

## Cross-field validation (v0.6)

JSON Schema validates one field at a time; many real-world
mistakes only become apparent when two fields disagree.
`scripts/alp_orchestrate/` runs after
the schema pass and enforces a small set of cross-field rules.
A violation raises `OrchestratorError`; warnings print to
`stderr` and let the load continue.

| #  | Rule                                                                                                    | Severity |
|----|---------------------------------------------------------------------------------------------------------|----------|
| 1  | `ota.provider: mender` requires at least one `cores.<id>.os: yocto` slice.                              | ERROR    |
| 2  | `boot.signing.algorithm:` must be in the SoM family's supported set (see table below).                  | ERROR    |
| 3  | `cores.<id>.iot.tls: true` requires `mbedtls` or `bearssl` in `libraries:` or `extra_libraries:`.       | ERROR    |
| 4  | `cores.<id>.inference.default_arena_kib` > `cores.<id>.memory.heap_kib` -- inference may OOM.           | WARN     |
| 5  | `cores.<id>.power.sleep_mode != disabled` with no `wakeup_sources:` declared -- device cannot wake.     | WARN     |

Rule 2 family-specific allow-list (built from the SoM preset's
`family:` field):

| Family             | Allowed `boot.signing.algorithm:` values             |
|--------------------|------------------------------------------------------|
| `alif-ensemble`    | `ecdsa_p256`, `ed25519`  (OPTIGA Trust M slot type)  |
| `renesas-rzv2n`    | `ecdsa_p256`, `rsa2048`, `rsa3072`                   |
| `nxp-imx9`         | `ecdsa_p256`, `rsa2048`, `rsa3072`                   |
| *(unknown family)* | Schema enum unrestricted (no capability data yet)    |

The warning rules (4 + 5) are informational: the build still
succeeds.  Tightening one of them to ERROR is a v0.7+ tightening
decision once enough field data confirms the heuristic.

## Versioning

There is no explicit `schema_version` field -- the schema at
`metadata/schemas/board.schema.json` is the single live shape, and
boards declare `cores:` (per-core runtimes + slices) even when only
one core is used.  Earlier schemas with a top-level `os:` were
removed when heterogeneous orchestration landed; see
[`docs/heterogeneous-builds.md`](heterogeneous-builds.md) for the
per-core walk-through.

## See also

- [`metadata/templates/board.yaml`](../metadata/templates/board.yaml)
  -- the canonical commented template.
- [`docs/heterogeneous-builds.md`](heterogeneous-builds.md)
  -- the multi-slice / `cores:` / `ipc:` walk-through.
- [`docs/recommended-libraries.md`](recommended-libraries.md)
  -- the curated library list `libraries:` draws from.
- [`docs/getting-started.md`](getting-started.md) -- the
  consumer-facing walkthrough.
- [`metadata/schemas/board.schema.json`](../metadata/schemas/board.schema.json)
  -- the authoritative schema.
