# Design — #673: split oversized orchestration, backend, console, and test modules

- **Issue:** alplabai/alp-sdk#673
- **Baseline:** `origin/dev` (review baseline `be80d54d`; work branch forks `origin/dev@8e9f1996`)
- **Work branch:** `refactor/673-split-oversized-modules`
- **Date:** 2026-07-11
- **Scope decided:** P1 + P2 (P3 public-header partitioning deferred)

## Goal

Cut per-file merge-conflict surface, review scope, and rebuild cost by splitting
the largest first-party source files into focused, independently understandable
units. This is a **behavior-preserving refactor**, never a rewrite. The value is
maintenance ergonomics, not aesthetics — so anything that is generated output,
vendor fork, or ABI surface is out of scope or handled by its own workflow.

## Non-goals

- No behavior change. Generated output stays byte-identical unless a schema
  change is separately reviewed.
- No public-symbol or shell-command-name changes.
- **P3 deferred:** public-header partitioning (`peripheral.h`, `chips/*.h`) is
  documented and non-urgent; revisit only if Doxygen/navigation metrics justify.
- No touching vendored (`vendors/`, `zephyr/drivers/` Tier-2 forks,
  `firmware/cc3501e/hal/ti/`) — those move by upstreaming/provenance/regen, not
  local refactor.

## Invariant spine (Definition of Done, applies to every phase)

A split is done only when all of the following hold:

- Generated output byte-identical (unless an intentional, reviewed schema change).
- Every public symbol and shell command name unchanged.
- Existing targeted suites green (179 tests + 48 subtests in
  `test_alp_orchestrate.py` / `test_alp_project.py`).
- Gates pass: `check_emit_snapshots.py`, metadata/schema gates, Doxygen, ABI
  snapshot, native-sim Twister, and the relevant CMake/link matrix.
- Each split records ownership/provenance in the affected module header.
- Target: Python functions < 300 physical lines and decision complexity < ~25,
  with documented exceptions for protocol tables and HW register drivers.

## Verified inventory (against `origin/dev`, 2026-07-11)

| File | Lines | Seam |
| --- | --- | --- |
| `scripts/alp_orchestrate/kconfig.py` | 1095 | `_slice_alp_conf` @402, 577L |
| `scripts/alp_orchestrate/loader.py` | 551 | `load_board_yaml` @210, 342L |
| `scripts/alp_project_emit.py` | 1666 | dispatcher + `_emit_dts_overlay` @692, 200L |
| `src/common/stub_backend.c` | 1738 | ~150 public symbols, `ALP_VENDOR_OVERRIDES_*` matrix |
| `src/zephyr/console/alp_console_companion.c` | 1554 | 31 `cmd_*` handlers; `cmd_companion_sock_tcp_get` ~290L |
| `tests/scripts/test_alp_orchestrate.py` | 3380 | 108 tests, 8 contracts |
| `tests/scripts/test_alp_project.py` | 1360 | 70 tests, emitter+schema+loader |
| `tests/zephyr/chips/src/main.c` | 2962 | >100 chip/block ztests, one TU |
| `firmware/gd32-bridge/tests/gen_protocol_vectors.py` | 736 | vector render + CLI |
| `firmware/cc3501e/tests/gen_protocol_vectors.py` | 206 | vector render + CLI |

Line counts confirmed by direct measurement; all named seams exist.

## Architecture of the splits

### Phase 0 — Safety net (PR 1, gate for all later phases)

Characterization coverage added **before** any production change:

- **Emit snapshot pin.** Capture byte-exact current output of `_slice_alp_conf`,
  `load_board_yaml`, and `alp_project_emit` for a representative board matrix
  (AEN E8 SKU, a V2N SKU, native_sim). Wire into the existing
  `check_emit_snapshots.py` path so any drift fails.
- **Stub compile/link matrix (new required CI gate).** Build `stub_backend`
  under: baremetal, Yocto, and each `ALP_VENDOR_OVERRIDES_*` combination
  (umbrella `PERIPHERAL` + per-class `I2C/SPI/GPIO/UART/...` and the independent
  `*_TARGET` / `*_RX_RINGBUF` sub-gates). Assert exactly one definition per
  symbol. This is the guarantee existing suites do **not** provide and is the
  precondition for Phase 2's C split. Lands as a CMake target and a required CI
  context.

### Phase 1 — Python orchestration splits (PRs 2–4, one per file)

All three keep their current public entry point; internals move behind it.

- **`kconfig.py::_slice_alp_conf`** → extract one section emitter per block —
  `_emit_console`, `_emit_som_caps`, `_emit_chips`, `_emit_libraries`,
  `_emit_inference`, `_emit_memory`, `_emit_power`, `_emit_storage`, `_emit_ota`,
  `_emit_diagnostics` — each returning a typed/list fragment, composed in a
  fixed-order pipeline. Sections already build section-local `_lines` lists, so
  extraction is mechanical. Preserve byte output and diagnostics ordering.
- **`loader.py::load_board_yaml`** → separate stages: YAML/schema load →
  board/SKU resolution → topology/core validation → storage resolution →
  final cross-field validation. Reuse existing helpers (`_validate_board`,
  `_resolve_board_preset`, `_synthesize_inline_board`, `_resolve_topology_for_core`,
  `_slice_from_resolved`). Keep one public `load_board_yaml`; preserve error
  precedence and messages exactly.
- **`alp_project_emit.py`** → new `alp_project_emit/` subpackage (decided over
  flat siblings) split by output contract behind the current dispatcher, sharing
  one immutable resolved model:
  - `emit/dts.py` — `_emit_dts_overlay`, `_emit_aen_adc_wiring`, board-macro
    parsing (`_board_header_path`, `_read_board_header_with_includes`,
    `_parse_board_macros`, `_project_pin_indices`, route/alias index helpers).
  - `emit/native_sim.py` — `_emit_native_sim_overlay`.
  - `emit/west_libs.py` — `_load_curated_library_manifest`,
    `_emit_west_libraries`, `_emit_library_hw_backends`.
  - `emit/hw_info.py` — `_pick_primary_core_os`, `_emit_hw_info_h`.
  - `emit/bom_netlist.py` — route/netlist + BOM cluster
    (`_composed_route_rows`, `_emit_composed_route_table`, `_manifest_path`,
    `_passive_rows`, `_chip_bom_row`, `_block_bom_row`, `_carrier_bom_rows`,
    `_route_to_net`, `_emit_carrier_netlist`).
  - Shared resolved model + dispatcher stay at the package root. Delete only
    proven-dead compatibility paths.

### Phase 2 — C splits (PR 5–6, blocked on Phase 0 link matrix)

- **`stub_backend.c`** → one translation unit per API class (`stub_i2c.c`,
  `stub_spi.c`, `stub_gpio.c`, `stub_uart.c`, `stub_pwm.c`, `stub_adc.c`,
  `stub_counter.c`, `stub_i2s.c`, `stub_can.c`, `stub_rtc.c`, `stub_wdt.c`, …)
  plus a shared error-slot/delay core TU. Retain exactly one definition per
  symbol and the full `ALP_VENDOR_OVERRIDES_*` override matrix, including the
  independent `*_TARGET` and `*_RX_RINGBUF` sub-gates. Do not delete the
  monolith until the Phase 0 matrix passes on the split.
- **`alp_console_companion.c`** → small core (companion context, locking, event
  and transport plumbing) plus command groups registered from separate files:
  - core: `ver`, `ping`, `reset`, `bench`
  - wifi (6): `wifi_scan/connect/disconnect/ap/ap_stop/status`
  - ble+gatt (13): `ble_*`, `gatt_*`
  - diag (3): `diag_info/stats/loglevel`
  - ota (3): `ota_status/begin/abort`
  - socket: `sock_tcp_get` (~290L, its own file)
  - v2n-gpio (2): `gpio_read/write`
  Keep shell command names and `CONFIG_*` behavior unchanged. Add
  command-registration tests.

### Phase 3 — P2 test/vector splits (PRs, one per group)

- **`test_alp_orchestrate.py`** → split by contract (`loader`, `storage`,
  `manifest`, `buildplan`, `dispatch`, `cache`, `security`, `extra_libs`); move
  board builders and assertion helpers into a shared fixture module. Keep pytest
  IDs stable where practical (CI required-context matching depends on them).
- **`test_alp_project.py`** → split emitter, schema, and loader cases; keep
  snapshot/fixture ownership explicit.
- **`tests/zephyr/chips/src/main.c`** → split by chip family / subsystem; keep a
  small common fake-bus helper. Coverage unchanged.
- **`gen_protocol_vectors.py` ×2** → extract a tiny neutral renderer for the
  shared text format into a common helper; leave protocol-specific vector
  construction in each firmware tree; retain generated-file checks.

## Execution model

- **6+ PRs**, following the issue's delivery order (Phase 0 → 3), each on a
  branch off `refactor/673-split-oversized-modules`, each independently
  reviewable and revertible.
- **Orchestrated parallel agents** (orchestrating-alp-sdk-agent-workflows): within
  a phase, file-disjoint splits fan across `alp-implementor` agents in isolated
  worktrees; a **serial local-CI gate** (running-local-ci) runs before each
  merge. C splits block on the Phase 0 link matrix.
- Phase 0 is strictly first and blocking. Phases 1 and 3 (Python + tests) may
  overlap since they are file-disjoint; Phase 2 (C) waits on Phase 0.

## Risks and mitigations

1. **stub override matrix** — a missed `#if` gate yields a duplicate or missing
   symbol at link. *Mitigation:* Phase 0 matrix lands first, exercises every
   combination, and is a required gate; monolith deleted only after it passes.
2. **Emit byte-drift** — comment/whitespace/ordering changes in extracted
   section emitters. *Mitigation:* Phase 0 snapshot pin + `check_emit_snapshots.py`.
3. **pytest ID churn** — renamed IDs break CI required-context matching
   (`project_ci_required_contexts_repointed`). *Mitigation:* keep IDs stable;
   verify required contexts still resolve after the test split.
4. **dev CI is partial on branches** — `pr-metadata-validate` and emit-snapshot
   gates run on `main` only (`project_pr_metadata_gate_main_only`); green branch
   CI is not full coverage. *Mitigation:* run the gates locally before each merge.
5. **Loader error-message drift** — reordering validation stages can change which
   error fires first. *Mitigation:* preserve error precedence; assert on exact
   messages in characterization tests.

## Open items resolved

- `alp_project_emit.py` layout → **subpackage** (`alp_project_emit/`), not flat
  siblings.
- Phase 0 stub matrix → **required CI gate**, not local-only.
