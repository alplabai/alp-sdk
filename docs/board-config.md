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

This page is the landing page (quick start, the single-source-of-truth
model, file location, cross-field validation, versioning).  Full
field-by-field detail lives in four focused reference pages:

- [`board-config-schema.md`](board-config-schema.md) -- the full
  `board.yaml` field reference: the `som:` / board split, inline vs.
  `preset:` mode, `pins:`, pin direction, the EVK-as-reference-design
  workflow, stock presets, and the `libraries:` block (ADR 0018).
- [`board-config-emit.md`](board-config-emit.md) -- how the loader
  compiles the file: `west alp-build`, the Zephyr `alp.conf` overlay,
  plain CMake, Yocto `local.conf`, `west.yml` auto-pin, the
  `hw-info-h` header, and the DTS overlay.
- [`board-config-hardware.md`](board-config-hardware.md) -- hardware
  revision tracking (build-time + runtime checks) and modular SoM
  chip populations.
- [`board-config-features.md`](board-config-features.md) -- the
  build-system integration knobs: `hw_info.eeprom`, per-slice
  memory/power, log levels, `boot:`, `ota:`, `storage:`, and
  `security.psa:`.

## Quick start: minimum-viable `board.yaml`

Paste this into `board.yaml` at your app root, change the `sku`
to your MPN, and you're done -- everything else is inherited from
the SDK's per-MPN preset:

```yaml
som:
  sku: E1M-AEN801        # your MPN -- the SDK ships a preset
                          # at metadata/e1m_modules/<MPN>.yaml

preset: e1m-evk          # or write your custom board out inline -- see
                          # board-config-schema.md#board-declaration

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
`populated:`, `e1m_routes:`) -- see the
[Board declaration](board-config-schema.md#board-declaration)
section of the schema reference.

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
  `.conf` file (it is a Kconfig-source directive only) -- see the
  worked example at `examples/peripheral-io/gpio-button-led/CMakeLists.txt`
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
features + optional libraries + Zephyr peripherals today.  DTS
overlays for board wiring (`--emit dts-overlay`) and `west.yml`
libraries auto-pinning (`--emit west-libraries`) were previously
tracked as v0.4 gaps; both are closed -- both ship in v0.3.  One
gap remains where hand-written config still leaks in, targeted for
v0.4:

1. **Per-test `prj.conf` in `tests/zephyr/<area>/`.**  The
   in-repo test infrastructure still uses hand-written
   `prj.conf` files.  These are SDK-internal (not consumer-
   facing) and stay as-is until the loader handles test-style
   configs in v0.4.

The loader's `--emit dts-overlay` mode parses
`include/alp/boards/<board>.h` and generates the bus aliases
(`alp-i2c<N>`, `alp-spi<N>`, `alp-uart<N>`, `alp-pwm<N>`) plus a
POSITIONAL `alp,pin-array` whose entry N is E1M pad N in the
canonical `e1m_pinout.h` order (52 slots: IO0..25 = 0..25, PWM0..7 =
26..33, ENC0_X..ENC3_Y = 34..41, ADC0..7 = 42..49, DAC0..1 = 50..51).
Per-pad GPIO bank/index columns remain TBD for SoMs without an
in-tree Zephyr board file yet under
[`zephyr/boards/alp/`](../zephyr/boards/alp/); the customer fills
those in place without renumbering.

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

- [`board-config-schema.md`](board-config-schema.md) -- the full
  `board.yaml` field reference.
- [`board-config-emit.md`](board-config-emit.md) -- how the loader
  compiles the file into per-backend build artefacts.
- [`board-config-hardware.md`](board-config-hardware.md) -- hardware
  revision tracking + modular SoM chip populations.
- [`board-config-features.md`](board-config-features.md) -- the
  build-system integration knobs (`boot:`, `ota:`, `storage:`,
  `security.psa:`, ...).
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
