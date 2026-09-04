# Closing three fragile points in the alp-sdk ↔ tan seam

Date: 2026-08-30
Status: Draft — for maintainer review
Scope owner: alpCaner

## Why this exists

A survey of the SDK↔CLI seam, an adversarial review of it, and a
mode-by-mode audit across both repos produced one uncomfortable result: the
seam is not what its documentation implies, and three specific places in it
break on changes that no schema and no gate can see.

This spec covers only those three. It deliberately does **not** decide
planner ownership; the ADR-0026 acceptance and its amendment are a separate
document, because the three fixes here hold their value whichever way that
decision goes.

## What the seam actually is (measured, 2026-08-30)

Both implementations are Python. `tan-cli` has no top-level `crates/`
directory; its planner lives at `tan-cli/python/tan/planner/` and is a
hash-pinned **port** of `alp-sdk/scripts/alp_orchestrate/**` plus
`scripts/alp_project_emit/**`, kept fresh by
`tan-cli/python/tests/gates/test_planner_relocation_freshness.py`.

All 20 `--emit` modes have a live consumer in tan. None is dead. None is
consumed as a schema-validated envelope:

| verdict | count | modes |
|---|---|---|
| REIMPL — tan re-implements the renderer against alp-sdk's raw metadata | 19 | `zephyr-conf`, `cmake-args`, `yocto-conf`, `dts-overlay`, `native-sim-overlay`, `hw-info-h`, `west-libraries`, `zephyr-board`, `composed-route-table`, `carrier-netlist`, `os-topology`, `ipc-contract-h`, `system-manifest`, `dts-reservations`, `dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`, `build-plan`, `kconfig` |
| VENDORED — byte-for-byte snapshot checked into tan | 1 | `scaffold` |

The only path on which tan runs the SDK's own emit code is the
`TAN_GENERATE_EXECUTOR=subprocess` escape hatch at
`tan-cli/python/tan/commands/generate_cmd.py:780-816`, documented as a path
nothing reaches by default.

The emit registry itself is healthy: `python3 scripts/check_emit_registry.py`
returns `OK (20 emit modes, in sync with the code)`, and
`metadata/emit-registry-v1.json`, both CLIs' `--emit choices=`, and
`docs/cli.md:302-310` agree exactly. The fragility is not registry drift.

## The rule this spec applies

From the adversarial review, and adopted here as the test for every change
below:

> **Export decisions, not rules.** For any rule, ask: (a) must an independent
> consumer apply it identically to be correct? If not, it stays in code.
> (b) Can the SDK evaluate it and ship the *outcome* in the artefact? If yes,
> ship the outcome — the rule never becomes anyone else's problem. (c) Only
> if the rule depends on consumer-side state the SDK cannot see must the rule
> itself cross, and then only as a closed vocabulary the schema fixes.
>
> Corollary for data-vs-code: if the rule is a **lookup**, it can be data. If
> expressing it as data requires inventing an **interpreter** for that data,
> it is mechanism and belongs in code.

## Problem 1 — `tan validate` parses the SDK's stderr with regexes

### What is there today

`tan validate` (the default path, absent `--offline`) spawns the SDK's own
validator: `VALIDATOR_SCRIPT = ("scripts", "validate_board_yaml.py")` at
`tan-cli/python/tan/commands/validate_cmd.py:319`, spawned at `:1497-1508`.
`tan diff` does the same through `_spawn_validator`
(`tan-cli/python/tan/commands/diff_cmd.py:608-628,678`).

It then recovers structure by **parsing that subprocess's stderr text** —
`_RICH_HEADER_RE`, `_ARROW_RE`, `_BLOCK_SEE_RE` and friends at
`tan-cli/python/tan/commands/validate_cmd.py:397-448` — to extract severity,
the `ALP-Bxxx` code, the hint and the `doc:` URL. The comment at `:437-441`
states the message body is passthrough, "taken from the validator's own
output rather than rebuilt from the code".

### Why it is fragile

A formatting change in the SDK's human-readable diagnostic output — a
reflowed line, a changed arrow glyph, an added prefix — breaks tan's field
extraction while every schema still validates green and every gate still
passes. There is no contract to violate, so nothing reports the break. This
is the single place in the seam where the interface is prose.

### Proposed change

Add a structured output mode to the SDK validator and have tan consume it:

- The exporter **already exists**: `scripts/alp_cli/diagnostic_format.py:109`,
  `machine_json_for_board_yaml()`, whose docstring describes it as
  "byte-identical to what `--format json` prints, but with no CLI in the call
  chain -- so a consumer ... binds to the exporter rather than to a command
  wrapper that ADR 0020 retires". So the work is not writing an exporter; it is
  deciding what door it gets.
- **This is not purely additive, and the spec should not claim it is.** Giving
  `scripts/validate_board_yaml.py` a `--format json` flag re-adds an SDK command
  wrapper that ADR 0020 end-state B deliberately removed (alp-sdk#1368). That
  reversal may well be right, but it has to be argued rather than slipped in.
  The alternative is for tan to bind the library door directly.
- **A new representation IS needed for half the output.** `diagnostic_format.py:122-125`
  states that only diagnostics are exported and that the `load_board_yaml`
  cross-field check is "a SEPARATE contract with no diagnostic-v1
  representation". That layer is `validate_board_yaml.py:80-91` -- the
  `FAIL sdk-compat:` / `FAIL consistency:` lines exiting 3/4/5, which tan
  recovers via `_FAIL_WARN_RE`. Those are the hardware-safety refusals in
  Risks below, so "no new schema is invented" is false precisely where it
  matters most. This answers Open question 1: **no**, `diagnostic-v1` does not
  cover them today.
- The human-readable output is unchanged and remains the default. This is
  additive; no existing invocation changes behaviour.
- `tan validate` / `tan diff` switch to `--format json` and drop the regexes.
  Severity, code, hint and doc URL become read fields rather than parsed text.
- Exit codes are untouched. `validate_board_yaml.py`'s 3/4/5 codes are part of
  the contract and stay as they are.

Against the rule: the SDK already computes the decision (this input is
invalid, for this reason, with this code). Today it *renders* that decision
and the consumer re-derives it. Emitting the decision is exactly "export
decisions, not rules".

### Rejected alternative

Harden tan's regexes. This moves the failure later, not away: the coupling to
prose remains, and a hardened regex fails in a more confusing way than a
loose one.

## Problem 2 — the class→runtime decision exists twice, and no schema enforces it

### What is there today

Two independent copies of the same rule:

- `alp-sdk/scripts/alp_orchestrate/topology.py:28-43` —
  `_default_os_from_core_type`, also reached from
  `scripts/alp_orchestrate/loader.py:535-555`.
- `tan-cli/python/tan/core/os_class.py:48-52,79-81` —
  `CLASS_RUNTIMES = ("yocto", "zephyr")`, `cortex-a` → yocto,
  `cortex-m` → zephyr.

An important correction to an earlier reading of this: `topology.py:64-70`'s
`_core_os_choices` already reads the `os` enum from `board.schema.json`, so
the *enum* is not duplicated. What is duplicated is the **class-determined
mapping** — which runtime a core class implies when the user has not chosen.
`metadata/schemas/board.schema.json`'s `$defs.core_entry.properties.os`
description does state the mapping in prose -- "The OS runtime is DERIVED from
the core's silicon class and is not selectable: Cortex-M -> Zephyr (RTOS),
Cortex-A -> Yocto (Linux)" -- but nothing expresses it in a machine-readable
form, and the same description says the cross-file check "is enforced in the
`alp_orchestrate` package ... since JSON Schema cannot see the SoC spec". It is not user-selectable, and any second
implementation must reproduce it exactly to be correct.

### Why it is fragile

This is the rule the parity apparatus pays for. It is a closed lookup with a
handful of entries, it is not consumer-state-dependent, and it is invisible to
every gate that validates the schemas.

### Proposed change

Move the mapping into metadata as a table, and have both sides read it rather
than restate it. Concretely:

- A declared core-class → default-runtime table in metadata, schema-backed,
  with each entry carrying the class prefix it matches and the runtime it
  implies.
- `scripts/alp_orchestrate/topology.py` reads the table instead of branching on
  string prefixes in Python.
- tan reads the same table. Because the table lives in alp-sdk's `metadata/`,
  which tan already reads live for all 19 REIMPL modes, this needs no new
  transport.
- A gate asserts every core class present in SoC metadata has exactly one
  entry, so a new SoM family cannot silently fall through to a default.

Against the rule: this is a lookup, not an algorithm. Expressing it as data
requires no interpreter. It passes.

### Explicitly not proposed

Codegen of the table into per-language constants. That is a reasonable
targeted fix in general and worth revisiting if a non-Python consumer appears,
but with both implementations reading the same filesystem it buys a build step
and no correctness.

## Problem 3 — `metadata/error-catalog.json` is generated from code and only half-consumed

### What is there today

The catalog is generated: `scripts/gen_error_catalog.py:5-14` derives it from
the `alp_status_t` enum in `include/alp/peripheral.h` plus the
`docs/diagnostics/ALP-B*.md` narratives. The `ALP-Bxxx` codes themselves are
string literals at roughly eleven raise sites in
`scripts/alp_cli/validator.py`.

Consumption in tan is split:

- `tan explain --code ALP-Bxxx` reads the catalog directly —
  `tan-cli/python/tan/core/error_catalog.py:1-20,33-35`
  (`CATALOG_RELATIVE = ("metadata", "error-catalog.json")`), invoked from
  `tan-cli/python/tan/commands/explain_cmd.py:935-957`.
- `tan validate` does **not** consult the catalog at all; it takes the message
  body from parsed stderr (Problem 1).

### Why it matters, and what is deliberately *not* changing

The adversarial review's verdict is adopted here: the catalog — code, meaning,
remediation — is a genuine cross-consumer contract, but the **raise sites** are
algorithmic. Which condition fires which code cannot move to data without
inventing an interpreter, and doing so would replace domain errors carrying
remediation with "constraint 47 violated". On a bench SDK where a wrong value
flashes the wrong module, that is not a cosmetic regression.

So the generate-from-code direction stays. Inverting the dependency buys
little.

### Proposed change

Harden the generated file into an actual contract:

- Validate `metadata/error-catalog.json` against a schema in CI, not merely
  regenerate-and-diff it.
- Gate that codes are never renumbered: a code, once published, keeps its
  meaning. Additions are additive; removals are explicit.
- With Problem 1 fixed, `tan validate` carries the code as a field and the
  catalog gains its second real consumer, which is what makes the contract
  worth having.

## Non-goals

Recorded so they are not re-litigated:

- **Gate allowlists stay in code.** `scripts/check_pin_conflicts.py:47`,
  `scripts/check_doc_drift.py:114`, `scripts/check_doc_links.py:77`. No
  external consumer must agree with a CI suppression list; a central
  exceptions file is a dumping ground.
- **A gate restating a schema rule is not drift.** Deliberate redundancy is
  what a gate is for. Only a *producer* restating a rule is drift. The 63
  `check_*.py` scripts are not a cleanup target.
- **`_ALLOWED_TYPES` stays.** `scripts/check_emit_kconfig_contract.py:49`
  restates Kconfig's own type system, an upstream Zephyr fact. A gate that
  reads the schema it checks loses its independence.
- **`_EXECUTION_POLICY` stays as it is.**
  `scripts/alp_orchestrate/buildplan.py:38-42` is already correct: it crosses
  the seam stamped into every plan, which is right because `missingTool` is
  consumer-side state the SDK cannot evaluate. It is also genuinely consumed —
  `tan-cli/python/tan/core/build_plan.py:124-154` parses all three keys,
  `python/tan/core/plan_exec.py:113` honours them at dispatch, and
  `python/tan/commands/build_cmd.py:1034,1330,1349,1467` plus
  `python/tan/commands/build/execute.py:32,141,890,1232` route skip/fail
  through it.
- **`_fix_link`'s regexes stay.** `scripts/alp_template.py:1262` and
  `:1444-1453`. The decisions (which core, which pins, which SKU) are already
  metadata-derived; the regexes are the application step, and each `subn`
  asserts its occurrence count and hard-errors on 0 or more than 1. Replacing
  them with `{{sku}}` holes would break the property that canonical examples
  are real, buildable, CI-tested projects — you would carry a real example and
  a templated twin, and they would drift. The one policy nugget worth
  extracting is the *target*: the base URL and the `v<version>`-else-`main`
  fallback around `scripts/alp_template.py:1324`. That is a small catalog
  field, and it is in scope only if it can be done without touching the
  rewrite machinery.
- **No embeddable policy engine.** CUE, Rego, Starlark or WASM are
  proportionate to a policy surface two orders of magnitude larger than a
  three-key execution policy and a core-class table. The cost is a runtime
  dependency in a firmware toolchain, debugging opacity, and a language nobody
  here writes.
- **No "pure data plus N interpreters".** That keeps the parity problem and
  relocates it: instead of two planners you get two interpreters of a rules
  file, now untyped and undebuggable.

## Risks

- **Problem 2 touches the loader.** `scripts/alp_orchestrate/topology.py` is
  reached from `loader.py`, which is also where the hardware-safety gate lives
  (see below). The change must not alter which revisions are refused.
- **Problem 1 changes a spawned interface.** tan pins an SDK commit; the
  `--format json` flag must land in alp-sdk and be released before tan can
  depend on it, or tan must fall back to the current parse when the flag is
  absent. Order matters: land the SDK flag first, then release, then switch
  tan, then delete tan's regexes.
- **A constraint none of the three changes may break: scaffold's seam must stay
  the rendered envelope.** `scaffold` is the one mode consumed as a
  byte-for-byte vendored snapshot rather than re-rendered — tan checks the
  captured output into `tan-cli/python/tan/templates/vendored/`, whose
  `MANIFEST.md:1-8` states it is "`alp-sdk --emit scaffold` output, captured
  byte-for-byte ... so `tan init`/`tan scaffold` can read it without ever
  shelling the SDK". That works because the SDK hands over finished bytes from
  `render_to_envelope` (`scripts/alp_template.py:1501-1520`). If scaffold ever
  follows the other nineteen modes into the "relocated renderer" pattern, the
  consumer has to port occurrence-checked regex surgery and per-SKU pin-rename
  derivation into its own process — which is a second parity apparatus, of
  exactly the kind ADR 0026 exists to retire. Keep the envelope.

- **Adjacent, and larger than this spec:** `scripts/alp_orchestrate/` is not
  only a planner. `SdkRevisionUnknown` / `SdkRevisionNotBuildable` /
  `SdkRevisionUnsupported` come from `alp_orchestrate.models` and are raised at
  `scripts/alp_orchestrate/loader.py:653,660` (unknown), `:702,709` (not
  buildable) and `:749` (unsupported), reached from `load_board_yaml` at
  `.../loader.py:1238-1260` on every emit, imported by
  `scripts/validate_board_yaml.py:21`,
  `scripts/gen_catalog.py:93,340,353-359`; sixteen `check_*.py` scripts import
  `alp_orchestrate` and six reach this refusal via `load_board_yaml`. That is
  the SDK's refuse-to-build-unbuildable-silicon enforcement, and
  `validate_board_yaml.py` is the very script tan spawns on its default
  validate path. Nothing in this spec removes it; it is recorded here because
  the ADR-0026 amendment must account for it.

## Open questions

1. Does `metadata/schemas/diagnostic-v1.schema.json` already express everything
   `validate_board_yaml.py` needs to emit (severity, code, hint, doc URL,
   source location), or does it need additive fields? To be answered by reading
   the schema before implementation, not assumed.
2. Where should the core-class → runtime table live — a new `metadata/` file
   with its own schema, or an additive block on an existing one?
3. Is the `_fix_link` base-URL extraction in scope now, or deferred? It is the
   smallest item and the least urgent.
4. `metadata/schemas/hw-revisions-v1.schema.json` is the only schema of the 27
   under `metadata/schemas/` with top-level `additionalProperties: true`. Its
   description ("Per-SoM-family hardware-revision compatibility table") gives no
   rationale for the exception. Is the open door deliberate — because a vendor's
   revision table carries fields the schema cannot enumerate ahead of time — or
   is it an oversight? The difference is whether a typo in a new SoM family's
   revision entry is caught or silently accepted, which matters because that
   table is the input to the SDK-version compatibility window. Not in scope
   here; it needs its own decision and probably its own issue.
## Decided during review — the unused `substitute` hook stays, because the schema already locks the door

`scripts/alp_template.py` declares an opt-in
`substitute: {"file": ..., "literal": ...}` mapping on a parameter spec (`:88`,
`:261`, `:270`), and no template in `metadata/templates/catalog-v1.json` uses
it — the usage count is zero. The adversarial review flagged this shape as the
embryo of an inner platform: the next template that needs a condition grows it a
condition field, then ordering semantics, and eventually it is an untyped
language interpreted once per consumer.

**Correcting how this was first framed here:** the earlier draft posed it as a
choice between deleting the hook and "recording in the catalog schema that it is
frozen", as though the enforcement were missing. It is not.
`metadata/schemas/template-catalog-v1.schema.json`'s `$defs.parameter` carries
`additionalProperties: false` with properties exactly
`constraints`, `default`, `description`, `name`, `type` — `substitute` is not
among them. The schema does not freeze the key; it **rejects** it. The
implementation's own docstring says so (`scripts/alp_template.py:311-313`: "the
schema forbids it -- additionalProperties: false"), and the hook is reachable
only from a synthetic test fixture.

That changes the verdict rather than weakening it. The inner-platform risk is
already contained by construction: nobody can grow the hook through the catalog
without first amending the schema, which is an explicit, reviewable decision
point rather than an accident. So:

- The hook **stays**. It is a tested extension point whose door is locked in the
  layer that validates customer-facing data.
- The lock **stays**. `$defs.parameter` keeps `additionalProperties: false`, and
  `substitute` stays out of its `properties`.
- The only work this decision implies is one sentence of prose in
  `$defs.parameter`'s description recording that the omission is deliberate —
  that `substitute` is implemented in `alp_template.py`, intentionally not
  admitted here, and that admitting it is a separate decision. Without that
  note, the next reader finds an implemented feature the schema silently
  refuses and reasonably assumes it is a bug.

Not implemented in this spec; it belongs to the implementation plan.

## Evidence

Every file:line in this document was read during the survey, the adversarial
review, or the cross-repo audit on 2026-08-30, against `alp-sdk` at
`origin/dev` = `bc6974d35` (citations rebased from the `00627b88` merge-base)
and `tan-cli` at `b9aa697`.
