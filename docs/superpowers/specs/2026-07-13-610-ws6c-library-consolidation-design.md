# #610 WS6-c — library-model consolidation

**Status:** design 2026-07-13 (full consolidation authorized). **Phased — high
build-break risk; native_sim + representative cross-compile verification is a
gate on the risky phase.**

## Problem

Two divergent library-selection models coexist:

- **Per-core** `cores.<id>.libraries:` — a hardcoded schema **enum** of ~24
  underscored tokens (`cmsis_dsp`, `libcoap`, `tflite_micro`, …) resolved by
  hardcoded Python dicts (`_LIBRARY_KCONFIG`, `_LIBRARY_WEST_MODULES` in
  `scripts/alp_project.py`) + a separate metadata tree
  `metadata/library-profiles/<name>/hw-backends.yaml`. No capability/RAM gating.
- **Project-wide** `libraries:` (top level, ADR 0018) — canonical hyphenated
  names resolved from `metadata/libraries/<name>.yaml` manifests
  (`scripts/alp_orchestrate/libraries.py`), with `requires:` capability/RAM/
  flash/core-class gating. **Dead in practice** — 0 examples use it.

Same library, two spellings (`cmsis_dsp` vs `cmsis-dsp`, `libcoap` vs `coap`).
26 example board.yaml all use the per-core form.

## Target (canonical = the manifest model)

One `libraries:` model backed by `metadata/libraries/<name>.yaml` (canonical
hyphenated names, `requires:`-gated), with **explicit per-entry core scope**.
Retire the per-core enum, the two hardcoded dicts, and the `library-profiles/`
tree (folding its HW-backend content into each manifest). The per-core
semantics become a manifest entry scoped to specific core IDs.

Unified `board.yaml` shape (schemaVersion 2):
```yaml
libraries:
  - name: cmsis-dsp        # canonical manifest name
    cores: [m55_hp]        # explicit scope; omit = every core whose OS the manifest supports
  - name: coap
    cores: [m55_hp]
```

## Phasing (each phase independently landable/verifiable)

### Phase 1 — additive, LOW risk (author the canonical surface)
1. Author a `metadata/libraries/<name>.yaml` manifest for every per-core token
   lacking one (~19: bearssl, catch2, coremqtt_sn, doctest, etl, fmt,
   gfx_compat, jsmn, libhelix, libwebsockets, littlefs, madgwick_ahrs, mbedtls,
   minimp3, opus, pid, tflite_micro, tinygsm, u8g2). Populate `integration:`
   from the existing `_LIBRARY_KCONFIG`/`_LIBRARY_WEST_MODULES` entries +
   `metadata/library-profiles/<token>/hw-backends.yaml`; `requires:` from known
   core-class/OS constraints. Validate each against `library-v1.schema.json`.
2. A canonical **alias table** `metadata/library-aliases-v1.json` (+ schema)
   mapping legacy per-core tokens → canonical manifest names
   (`cmsis_dsp`→`cmsis-dsp`, `libcoap`→`coap`, `nlohmann_json`→…). Drives the
   migration + a temporary read-compat in the loader.
3. Gate `check_library_registry.py`: every per-core enum token + every existing
   board.yaml library reference resolves to exactly one manifest (via the alias
   table). This makes the coverage gap machine-visible before any rewrite.

### Phase 2 — schema + migration, MEDIUM risk
4. `board.schema.json` schemaVersion 2: add the unified `libraries: [{name,
   cores?}]`; keep reading the legacy per-core enum + top-level array as v1
   (absent schemaVersion ⇒ v1). No enum removal yet.
5. `alp-migrate` migration **m001_to_v2** (the first REAL alp-migrate v1→v2 —
   exercises the WS6-b engine): rewrite each `cores.<id>.libraries:
   [token,...]` into top-level `libraries: [{name: <canonical>, cores: [<id>]}]`
   via the alias table, byte-faithfully. Apply to the 26 repo board.yaml.

### Phase 3 — resolution rewrite + retirement, HIGH risk (bench/native_sim gated)
6. Rewrite `scripts/alp_orchestrate/{loader,kconfig,libraries}.py` +
   `scripts/alp_project.py` to resolve the unified model through the manifest
   layer only; delete `_LIBRARY_KCONFIG`, `_LIBRARY_WEST_MODULES`,
   `_emit_library_hw_backends`, the per-core enum, and the `library-profiles/`
   tree (fold HW-backends into manifests). No compat aliases left (per epic).
7. **Verification gate (mandatory before merge):** every one of the 26 examples
   emits byte-identical (or intentionally-diffed) `alp.conf`/`west-libraries`
   pre/post; native_sim twister green across the display/AI/connectivity
   examples that pull libraries; representative real cross-compiles per family.
   Per [[feedback_bench_before_merge]] any silicon-affecting delta stops for a
   human flash-gate.

## Non-goals
- No compatibility aliases surviving past the migration (epic §6 rule).
- No change to library *content* — only the selection/resolution model.

## Acceptance
- [ ] Every per-core token has a canonical manifest; alias table complete + gated.
- [ ] board.yaml schemaVersion 2 + m001_to_v2 migration; 26 examples migrated.
- [ ] Single manifest-backed resolver; hardcoded dicts + library-profiles/ + enum retired.
- [ ] Pre/post emit parity proven on all 26 examples; native_sim green; docs updated.

## Risk note
Phase 3 rewrites core build-config emission. It MUST be gated on the emit-parity
harness (step 7) — a silent CONFIG_ drop would break real firmware. Land Phase 1
first (pure additive), then 2, then 3 behind the parity gate. Do NOT collapse
the phases into one PR.
