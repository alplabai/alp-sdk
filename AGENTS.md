# AGENTS.md — working in alp-sdk as an AI agent

Vendor-neutral guidance for **any** coding agent (Cursor, Codex, Copilot,
Claude, …) editing this repo. Read it before generating code. **This file is
the authoritative agent guide** — it is self-contained and assumes no tooling
beyond a checkout (see the note at the bottom for the optional Claude Code
extras).

## What the SDK is

alp-sdk is a unification layer for **E1M** and **E1M-X** system-on-modules — a
heterogeneous mix of MCU and MPU silicon from **Alif (Ensemble)**, **Renesas
(RZ/V2N)**, and **NXP (i.MX 93)**. It rides *on top of* each vendor SDK rather
than replacing it. Cortex-M cores run **Zephyr**; Cortex-A cores run **Yocto
Linux** — the runtime follows the core class, it is not a user choice. Apps
write against one portable C API and stay source-compatible across every SoM in
a family. E1M and E1M-X are **separate product lines** (different SoCs, separate
`<alp/*_pinout.h>` namespaces); there is no cross-family portability.

## Golden rule for app / example code

App and example code uses the **portable `<alp/*>` API plus E1M instance IDs
ONLY**:

- Include `<alp/peripheral.h>`, `<alp/adc.h>`, `<alp/pwm.h>`, … and call
  `alp_<class>_open()` / `alp_gpio_open()` / `alp_last_error()` (returns
  `ALP_OK` on success).
- Name instances with the portable IDs from `<alp/e1m_pinout.h>`
  (`ALP_E1M_I2C0`, `ALP_E1M_PWM3`, `ALP_E1M_GPIO_IO15`, …) or, for E1M-X, the `ALP_E1M_X_*`
  IDs from `<alp/e1m_x_pinout.h>`.
- **A Ring-1 (cross-family, general/peripheral-agnostic) example never
  includes a chip driver** — symbols like `gd32g553_*`, `alif_*`, `lsm6dso_*`
  are SDK backends, not portable API, and pulling one into a Ring-1 example
  breaks its cross-family build and trips
  `scripts/check_example_portability.py`. Ring-2 (chip-bound) and Ring-3
  (SoM-bound) examples are an accepted, intentional category, not
  portability debt: a chip bring-up demo or single-sensor/single-display
  tutorial is *supposed* to `#include <alp/chips/<chip>.h>` directly and
  declare that chip in `board.yaml`'s `chips:` list — that's how the example
  teaches the specific part. See `docs/portability.md` §4.4 /
  `examples/README.md` for the customer-facing statement of this contract.

The only board-side names an app references are the C macros generated from
`board.yaml`'s `e1m_routes:` (e.g. `EVK_PIN_LED_RED`, `I2C_BUS_SENSORS`), each
of which resolves to an `ALP_E1M_*` ID (`ALP_E1M_X_*` for an E1M-X board) —
see `scripts/gen_board_header.py`'s `_c_token()`.

## The board.yaml → orchestrator flow

`board.yaml` (one per app, at the app root) is the **single source of truth**
for what a firmware project targets: SoM SKU, board, and per-core
apps/libraries/peripherals. Every other config artefact — Zephyr `prj.conf`,
CMake `-D` args, Yocto `local.conf` — is **derived** from it; do not hand-edit
those. Silicon-determined facts (NPUs, on-module memory/components, the OS per
core) come from the SoM preset under `metadata/e1m_modules/<MPN>.yaml` and are
**not** customer-facing knobs.

Two loaders fan `board.yaml` into per-core slices:

- `scripts/alp_project.py --emit {zephyr-conf,cmake-args,yocto-conf,hw-info-h,dts-overlay,west-libraries,os-topology,native-sim-overlay,system-manifest,dts-reservations,ipc-contract-h,composed-route-table,carrier-netlist,zephyr-board,scaffold}`
  — the per-slice build config.
- `python -m alp_orchestrate --emit {system-manifest,build-plan,ipc-contract-h,dts-reservations,dts-partitions,storage-mounts-c,tfm-sysbuild-conf,kconfig}`
  — the cross-core / system artefacts.

`tan build --project <app-dir>` is the customer wrapper. The current Python
implementation carries a relocated in-process planner, reads the selected
alp-sdk checkout's metadata/schemas, materialises the per-slice config, then
runs each slice's native build command. The SDK's own `--emit build-plan` and
`--emit system-manifest` remain the inspectable parity/reference producer while
the port settles; emit them directly when reviewing what a `board.yaml`
resolves to.

The `--emit` surface is the **machine-readable contract** other tools consume
(ADR 0014, `docs/adr/0014-build-plan-emit-cli-contract.md`). When you need to
know what a `board.yaml` resolves to, emit `system-manifest` / `build-plan`
rather than guessing.

## How to validate your work (generate → validate → fix)

An agent's loop here is: generate, then run the validators, then fix what they
report.

- `tan doctor` — HW-free build/flash-readiness preflight: checks `west`,
  `cmake`, `ninja`, the pinned Zephyr version, the Zephyr SDK, etc., and
  prints a remediation hint per failing check (`--format json` for machine
  consumption; there's no `--strict`). Run it first on a fresh checkout to
  find why a build won't work before you build. `--build` remains accepted
  for v0.4 compatibility but no longer changes the check list.
- `tan validate --offline --board-yaml board.yaml` — Python Tan's bundled
  structural checks, without resolving an SDK. Without `--offline`, Tan
  resolves the SDK checkout and spawns its full validator itself (ported in
  tan-cli#376, shipped in tan v0.6.0). The same validator can also be run
  directly, without Tan: `python3 scripts/validate_board_yaml.py --input
  board.yaml` — note the flag is `--input`, not `--board-yaml`. Try it against
  a fixture under `tests/fixtures/board_yaml_bad/` to learn the output
  format.
- `python -m alp_orchestrate --input board.yaml --emit build-plan` — the SDK
  reference validation/planning path used for parity before compile work.
- CI gates — `scripts/check_*.py` (e.g. `check_doc_drift.py`,
  `check_example_portability.py`, `check_pin_conflicts.py`,
  `check_system_manifest.py`) plus **twister** for the Zephyr ztest + example
  suite under `native_sim`. Run `bash scripts/test-all.sh` for the full local
  pass; `scripts/bootstrap.sh` sets up a fresh checkout's environment.

## Conventions you must respect

- **Chip-driver naming.** Drivers under `chips/<part>/` use the part's
  **natural name** (`lsm6dso_init()`, not `alp_lsm6dso_*`). The `alp_` prefix is
  **reserved for SDK abstractions** (the portable `<alp/*>` API).
- **No legacy compat.** There are no active external customers; delete removed
  code cleanly. No ABI shims, no tombstones, no deprecation aliases. Update
  dependent code (dispatch, backends, examples, tests, docs) in the same change.
- **Examples are documentation.** Each example's `src/main.c` — nearly always
  nested under a family directory (`examples/<family>/<name>/src/main.c`,
  e.g. `examples/v2n/v2n-temp-sensor/src/main.c`), not directly under
  `examples/<name>/src/main.c` — is a teaching artefact (~50% comments) for
  hand-written firmware — keep that density; don't strip the explanatory
  comments.
- **Metadata is single-source.** Every hardware fact has exactly one home under
  `metadata/`. Don't duplicate it into headers, docs, or code — duplicated
  truth is a bug. Downstream files are generated from `metadata/`.
- **Portable API stays vendor-clean.** No `gd32g553_*` / `alif_*` / vendor
  symbols leak into `<alp/*>` headers or app code.

## Where to look

- `metadata/` — the hardware truth: SoM presets (`e1m_modules/`), board presets
  (`boards/`), SoC capability JSON (`socs/`), and `schemas/board.schema.json`.
- `include/alp/` — the portable C API (Doxygen-commented public headers).
- `examples/` — minimal per-peripheral apps + end-to-end reference apps, each
  driven by a `board.yaml`.
- `docs/` — guides: `getting-started.md`, `board-config.md`, `architecture.md`,
  `e1m-pinout.md`, `portability-matrix.md`, the generated
  `peripheral-support-matrix.md` (SoM × peripheral presence), the ADRs in
  `docs/adr/`.
- `scripts/` — the loaders, validators, and `check_*.py` CI gates.

## A note for Claude Code users

Alp Lab's own Claude Code checkouts carry a local, untracked `CLAUDE.md` that
wires up two **internal-only** conveniences: a workflow-procedure skills plugin
and a `code-review-graph` MCP server for structural code queries. Both live in
Alp Lab's private tooling and are **not distributed with this repo** — if you
are reading this from a public checkout, you do not have them and do not need
them. Nothing in this repo depends on either: every fact an agent needs is
reachable from **this** file and the `docs/` it points to.
