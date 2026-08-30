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

- `scripts/validate_board_yaml.py` gains `--format json`, emitting a document
  that validates against the **existing**
  `metadata/schemas/diagnostic-v1.schema.json`. No new schema is invented; the
  repo already has the versioned machine-diagnostics document type.
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

## Problem 2 — the class→runtime decision exists twice, and no schema states it

### What is there today

Two independent copies of the same rule:

- `alp-sdk/scripts/alp_orchestrate/topology.py:28-43` —
  `_default_os_from_core_type`, also reached from
  `scripts/alp_orchestrate/loader.py:535-555`.
- `tan-cli/python/tan/core/os_class.py:48-52,79-81` —
  `CLASS_RUNTIMES = ("yocto", "zephyr")`, `cortex-a` → yocto,
  `cortex-m` → zephyr.

An important correction to an earlier reading of this: `topology.py:64-70`'s
`_load_board_os_enum` already reads the `os` enum from `board.schema.json`, so
the *enum* is not duplicated. What is duplicated is the **class-determined
mapping** — which runtime a core class implies when the user has not chosen.
No schema states that mapping. It is not user-selectable, and any second
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
- **`_fix_link`'s regexes stay.** `scripts/alp_template.py:1213` and
  `:1395-1404`. The decisions (which core, which pins, which SKU) are already
  metadata-derived; the regexes are the application step, and each `subn`
  asserts its occurrence count and hard-errors on 0 or more than 1. Replacing
  them with `{{sku}}` holes would break the property that canonical examples
  are real, buildable, CI-tested projects — you would carry a real example and
  a templated twin, and they would drift. The one policy nugget worth
  extracting is the *target*: the base URL and the `v<version>`-else-`main`
  fallback around `scripts/alp_template.py:1275`. That is a small catalog
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
- **Adjacent, and larger than this spec:** `scripts/alp_orchestrate/` is not
  only a planner. `SdkRevisionUnknown` / `SdkRevisionNotBuildable` /
  `SdkRevisionUnsupported` come from `alp_orchestrate.models` and are raised at
  `scripts/alp_project_loader.py:436-457`, imported by
  `scripts/validate_board_yaml.py:21`,
  `scripts/gen_catalog.py:93,340,353-359` and seven `check_*.py` gates. That is
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

## Evidence

Every file:line in this document was read during the survey, the adversarial
review, or the cross-repo audit on 2026-08-30, against `alp-sdk` at
`origin/dev` = `00627b88` and `tan-cli` at `b9aa697`.
