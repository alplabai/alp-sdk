# AGENTS.md — working in alp-sdk as an AI agent

Vendor-neutral guidance for **any** coding agent (Cursor, Codex, Copilot,
Claude, …) editing this repo. Read it before generating code. It is the
cross-tool companion to `CLAUDE.md` (Claude-Code-specific tooling lives there;
see the note at the bottom).

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
  (`E1M_I2C0`, `E1M_PWM3`, `E1M_GPIO_IO15`, …) or, for E1M-X, the `E1M_X_*`
  IDs from `<alp/e1m_x_pinout.h>`. (There is no `ALP_E1M_*` prefix.)
- **Never include a chip driver in app/example code** — symbols like
  `gd32g553_*`, `alif_*`, `lsm6dso_*` are **SDK backends and bridge demos**,
  not application API. Pulling them into an app breaks portability and trips
  `scripts/check_example_portability.py`.

The only board-side names an app references are the C macros generated from
`board.yaml`'s `e1m_routes:` (e.g. `EVK_PIN_LED_RED`, `I2C_BUS_SENSORS`), each
of which resolves to an `E1M_*` ID.

## The board.yaml → orchestrator flow

`board.yaml` (one per app, at the app root) is the **single source of truth**
for what a firmware project targets: SoM SKU, board, and per-core
apps/libraries/peripherals. Every other config artefact — Zephyr `prj.conf`,
CMake `-D` args, Yocto `local.conf` — is **derived** from it; do not hand-edit
those. Silicon-determined facts (NPUs, on-module memory/components, the OS per
core) come from the SoM preset under `metadata/e1m_modules/<MPN>.yaml` and are
**not** customer-facing knobs.

Two loaders fan `board.yaml` into per-core slices:

- `scripts/alp_project.py --emit {zephyr-conf,cmake-args,yocto-conf,hw-info-h,dts-overlay,west-libraries,os-topology}`
  — the per-slice build config.
- `scripts/alp_orchestrate.py --emit {system-manifest,build-plan,ipc-contract-h,dts-reservations,dts-partitions,storage-mounts-c,tfm-sysbuild-conf}`
  — the cross-core / system artefacts.

`west alp-build -b <board> <app-dir>` is the convenience wrapper: it validates
`board.yaml`, generates the build-time config, then delegates to `west build`.

The `--emit` surface is the **machine-readable contract** other tools consume
(ADR 0014, `docs/adr/0014-build-plan-emit-cli-contract.md`). When you need to
know what a `board.yaml` resolves to, emit `system-manifest` / `build-plan`
rather than guessing.

## How to validate your work (generate → validate → fix)

An agent's loop here is: generate, then run the validators, then fix what they
report.

- `alp validate board.yaml` — the diagnostic-rich `board.yaml` validator
  (CLI entry `alp`; equivalently `python3 scripts/validate_board_yaml.py`).
  Try it against a fixture under `tests/fixtures/board_yaml_bad/` to learn the
  output format. Exit code 3 on a hard failure.
- `west alp-build …` — does the same validation as a build pre-flight before
  any compile work.
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
- **Examples are documentation.** Each `examples/*/src/main.c` is a teaching
  artefact (~50% comments) for hand-written firmware — keep that density; don't
  strip the explanatory comments.
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
  `e1m-pinout.md`, `portability-matrix.md`, the ADRs in `docs/adr/`.
- `scripts/` — the loaders, validators, and `check_*.py` CI gates.

## A note for Claude Code users

This repo also carries `CLAUDE.md`, which adds Claude-Code-specific tooling: a
set of `.claude/skills/` workflow procedures and a `code-review-graph` MCP
server for structural code queries. Those are Claude-Code conveniences — every
fact an agent needs is reachable from this file and the `docs/` it points to.
Other agents should use **this** file; the two are complementary, not redundant.
