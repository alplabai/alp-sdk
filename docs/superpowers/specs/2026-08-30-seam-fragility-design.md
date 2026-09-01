# Closing four fragile points in the alp-sdk ↔ tan seam

Date: 2026-08-30
Status: Draft — for maintainer review
Scope owner: alpCaner

## Why this exists

A survey of the SDK↔CLI seam, an adversarial review of it, and a
mode-by-mode audit across both repos produced one uncomfortable result: the
seam is not what its documentation implies, and four specific places in it
break on changes that no schema and no gate can see.

Three of the four were in the first draft. Problem 4 — `build-plan-v1`'s key
names crossing three repos with no cross-repo gate — was found in review of
that draft and added here; the review's own verdict was that it is "the same
class as Problem 1", and it is.

This spec covers only those four. It deliberately does **not** decide
planner ownership; the ADR-0026 acceptance and its amendment are a separate
document, because the four fixes here hold their value whichever way that
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

## "Policy metadata" is two different things wearing one name

Added after review, because the phrase this work has been carried out under —
"policy metadata" — conflates a hardware fact with a decision, and the
conflation is already in the tree, not only in the vocabulary.

### The category

`metadata/chips/`, `metadata/socs/`, `metadata/e1m_modules/`,
`metadata/boards/`, `metadata/pinmux/` and `metadata/blocks/` are **hardware
truth**: they change when a datasheet or a schematic changes, and no decision of
ours can make them say otherwise.

These, also under `metadata/`, are not:

| File | What it actually records, in its own words |
|---|---|
| `metadata/emit-registry-v1.json` | "Single source of truth for every `--emit` mode the SDK exposes across `scripts/alp_project.py` and `scripts/alp_orchestrate/`" |
| `metadata/quality-tasks-v1.json` | "…which `check_*.py` gates exist, whether each is a hard CI gate or informational, and which profiles…" |
| `metadata/toolchains.json` | the Zephyr SDK pin and a measured footprint |
| `metadata/bootstrap.json` | `zephyr`, `venv`, `prerequisites`, `artifactProvenance` |
| `metadata/library-aliases-v1.json` | legacy-token to current-name alias table |
| `metadata/registries/` | `peripheral-kconfig.json`, `silicon-kconfig.json`, `tier-a-library-ci.json` |

Every row is a **decision**: it changes when we decide differently, not when the
silicon does. `quality-tasks-v1.json` is already a decision table in the exact
sense this section is about — "hard gate or informational" is policy, schema-
backed, under `metadata/`.

So the objection "this is not metadata" is correct about the *category* and
wrong if it is taken to mean the artefact cannot live under `metadata/`. The
directory is already a transport for both kinds, and transport is a real reason:
tan reads `metadata/` live for all 19 REIMPL emit modes, so anything placed
there crosses the seam with no new mechanism. What the distinction must change
is **naming and schema framing** — a decision file says "SDK policy, versioned,
changes by decision"; a SoC file says "silicon fact, changes when the datasheet
does" — not the path.

### The consequence for ADR-0026

`docs/adr/0026-tan-owns-the-planner-outright.md` clause 2 keeps `metadata/`,
`metadata/schemas/`, examples and the tooling contracts in alp-sdk because
"Those are hardware truth and stay in the repo that owns the hardware. The
planner is not hardware truth; it is a consumer of it."

That justification does not describe half of the directory it justifies, and
clause 2 is load-bearing: it decides what stays in alp-sdk when the planner
leaves. The decision itself still looks right — a tooling contract belongs with
the tool that publishes it — but the *stated reason* covers only the hardware
half, so the other half currently stays for a reason nobody has written down.
Worth an amendment sentence; not a reversal.

## What a rule looks like when it becomes data — and why most should not

### Prong (b) first: the usual answer is that no rule crosses at all

The test above is ordered, and the ordering is the point. Reaching for a table
is reaching for prong (c). Before that, (b) asks whether the **outcome** can be
recorded once instead of the rule being applied over and over — and for the
class-to-runtime mapping it can, so the right artefact is not a decision table
at all, it is a decided **field**.

**A wording correction, because the first version of this paragraph blurred two
different mechanisms.** Prong (b) is usually read as "the SDK evaluates the rule
at emit time and stamps the result into the artefact", which is what
`_EXECUTION_POLICY` does. That is *not* what is proposed here. Here the decision
is **authored** into the SoM preset by a human and read back; the SDK evaluates
nothing on the build path, and the class rule survives only as a producer-side
CI assertion. Both shapes satisfy (b) — the rule stops being any consumer's
problem — but only the second removes evaluation altogether, which is the actual
answer to "why is the SDK applying this rule at all". Today it applies it
because the field was made optional and 26 of 26 presets left it blank, so the
fallback became the mechanism.

The evidence that this is not hypothetical: `som-preset-v1.schema.json` already
declares the field. `$defs/topology_entry/properties/os` carries the description
"Default runtime for this core.  Customer's board.yaml `cores.<id>.os`
overrides." And `scripts/alp_orchestrate/loader.py:555` already prefers it:

```python
os=str(entry.get("os") or _default_os_from_core_type(soc_core_type)),
```

**Measured: all 26 `topology.<core>` entries across the eleven
`metadata/e1m_modules/*.yaml` presets omit `os`.** The schema's
`$defs.topology_entry` has `required: []`, so every one of them validates.

Be precise about which half is unused, because the imprecise version invites a
deletion that would break 53 projects. `entry` at `loader.py:555` is not the
preset entry — it is the merged dict from `_resolve_topology_for_core`
(`scripts/alp_orchestrate/loader.py:165-179`), where a customer's own
`board.yaml cores.<id>.os` is layered over the preset's `topology.<id>`. **53
in-tree `board.yaml` files set `cores.<id>.os`** (56 occurrences: 54 `"off"`,
2 `zephyr`), e.g. `examples/aen/aen-analog-validate/board.yaml:27`, and every
one of those takes the LEFT branch. So the left-hand branch is exercised
constantly by customers; it is the **preset** field that has never been
populated, which is what makes the prefix rule the de-facto mechanism for any
core a customer does not name.

So the change is small, and it is not a new file:

- Populate `os` on all 26 preset topology entries.
- Make it `required` in `$defs.topology_entry`, so a new SoM family cannot omit
  it. Absence becomes a validation failure — the totality guarantee Problem 2
  wanted, obtained without a 28th schema.
- Keep the prefix rule as a **producer-side gate only**, asserting the declared
  field agrees with the core class. This spec's own non-goals already bless
  exactly that: "A gate restating a schema rule is not drift. Deliberate
  redundancy is what a gate is for. Only a *producer* restating a rule is
  drift."

Then no consumer applies a rule in any language. Python, Rust and TypeScript
each read a field. There is no matching semantics to reimplement, no case
folding, no fall-through — the portability problem is dissolved rather than
adjudicated.

While there: `som-preset-v1.schema.json` re-types the `os` value set a fourth
time (`["yocto", "zephyr", "baremetal", "off"]`) instead of referencing
`board.schema.json`, and narrows it a fifth time at
`/allOf[0]/if/properties/topology/additionalProperties/properties/os` to
`["zephyr", "baremetal"]`. `topology.py:49`'s `_core_os_choices` already derives
rather than re-types; the schema should too.

### Prong (c): the shape for the residue

Some rule genuinely will depend on consumer-side state the SDK cannot see —
`_EXECUTION_POLICY`'s `missingTool` is the existing example, and it crosses
correctly today.

**The governing decision for that residue is now
[ADR 0033](../../adr/0033-support-policy-is-declared-data-with-one-normative-evaluator.md)**
(Proposed): alp-sdk declares support policy as data, and exactly **one**
normative evaluator turns a declared policy into a verdict — consumers call it
or carry a hash-pinned audited port, never a prose re-derivation. That evaluator
stays in alp-sdk `scripts/`, because 96 configure-time `CMakeLists.txt`
invocations and the four `west alp-*` commands reach `load_board_yaml` with no
tan in the process; an evaluator they cannot reach would silently drop the
refusal from every plain `west build`. One evaluator is also what keeps this
compatible with the non-goals below: "no embeddable policy engine" and "no pure
data plus N interpreters" both stand unchanged, since the harm those name is
*two* interpreters.

ADR 0033 also settles a framing this spec was carrying loosely: a refusal that
enforces a **support decision** must say so rather than assert a hardware limit.
The class refusal at `scripts/alp_orchestrate/validate.py:270-282`, reached from
`loader.py:910`, tells the customer its runtime "is determined by the core
class" — but upstream Zephyr at the pinned v4.4.1 ships
`arch/arm/core/cortex_a_r/`, so Zephyr on Cortex-A is a combination alp-sdk
*chose* not to support, not one the silicon forbids. Rewritten for customers in
`docs/board-config-schema.md`.

For that residue, and only for it, the schema must impose all of the following.
Each one removes a degree of freedom that a second implementation could
otherwise resolve differently:

1. **Exact values only — no prefix, no glob, no regex anywhere in the
   contract.** Matching semantics are the thing another language reimplements
   wrong. A regex is an interpreter, which the corollary above already forbids.
2. **Closed codomain, derived and not re-typed.** The outcome enum references
   the one schema that owns those values, gated so the two cannot drift.
3. **Required, `additionalProperties: false`, and a `schemaVersion`.** Absence
   must fail validation, never fall through to a default.
4. **No `default` or catch-all entry.** A default is precisely the thing that
   silently absorbs the case nobody thought about.
5. **Totality and non-overlap proved by a gate, not by ordering.** No
   "first match wins". Ordering is a hidden rule, and a hidden rule is what the
   second implementation gets wrong — #320 recurring as #485 is that story. Once
   a gate proves the table total and non-overlapping over a domain enumerated
   from *existing* metadata, order carries no meaning and cannot be got wrong.
6. **Normative miss semantics stated in the schema description**: an
   unresolvable input yields *unresolved*, never a guessed value.

Over-engineering, rejected explicitly: priority or ordering fields; per-entry
effectivity or version ranges; conditions or predicates of any kind; and any
specification of case folding — require canonical lowercase in the data and gate
it instead, because a locale-dependent fold (Turkish dotless `i` against ASCII
`i`) is a real divergence vector, eliminated by never folding rather than by
specifying how to fold.

### What this does not fix, stated plainly

It ends **mapping** drift, not all drift. The two implementations have already
diverged in the code *around* the lookup, and no schema reaches that:

```
alp-sdk  _default_os_from_core_type('')  ->  'off'
alp-sdk  _allowed_os_for_core('')        ->  ['baremetal', 'off']
alp-sdk  _default_os_from_core_type(5)   ->  AttributeError: 'int' object has no attribute 'lower'

tan      allowed_os_for_core('')         ->  []
tan      allowed_os_for_core(5)          ->  []
```

tan guards at `tan-cli/python/tan/core/os_class.py:127-128`
(`if not isinstance(core_type, str) or not core_type: return []`, per
tan-cli#914 / tan-cli#957) and its docstring rejects the other answer by name —
"the identical `["baremetal", "off"]` plausible-but-wrong guess this docstring
already rejects for the empty-string case".
`scripts/alp_orchestrate/topology.py:96-101` had no such guard. tan took the
fix; alp-sdk did not. It was in the one direction the freshness gate cannot see,
because tan's copy sits outside `tan/planner/` and outside `HAND_PORT_SOURCES`.
Only having one implementation removes that class, which is ADR-0026's
argument, not a schema's.

**Status, so this paragraph does not rot into a false claim.** The instance was
filed as [#1852](https://github.com/alplabai/alp-sdk/issues/1852) and is fixed
on `dev`: #1888 merged 2026-09-01 as `f5c7ff5b`, and
`scripts/alp_orchestrate/topology.py` now carries three
`isinstance(core_type, str)` guards with no bare
`(core_type or "").lower()` left in code. It found the divergence wider than
filed: the bare
`(core_type or "").lower()` idiom was in three functions, not two, and
`core_os_topology`'s `soc_types` comprehension passed a non-string `type`
straight into the emitted `core_type` field as well as into the crash. Closing
one instance does not close the class — that is this paragraph's point, and the
next instance will arrive the same way until there is one implementation.

### Sequencing — this question is downstream of ADR-0026 sections C and D

Where this artefact lives depends on an unresolved decision. ADR-0026's
amendment section C (who answers the configure-time CMake call) and section D
(who owns rendered-artefact bytes) determine whether both repos still need this
answer at all. If they land on "tan owns the renderers and alp-sdk's emitters
die", the honest end state is one function in one repo, and a schema, a gate, a
required field and a migration would be ceremony added in the middle of a
programme whose whole purpose is removing ceremony. **Answering the shape before
C and D is answering out of order.** What is safe now regardless: populate the
26 fields, since a preset stating its own core's runtime is correct under every
outcome.

### A correction to this document's own arithmetic

An earlier count in this work claimed that 18 entries fall through
`_default_os_from_core_type`'s `anything else -> off` arm. They do not. Those 18
(`ethos-u55` x12, `ethos-u85` x3, `ethos-u65` x1, `drp-ai` x1, `deepx-dx-m1` x1)
are `npus[]` entries, and `core_os_topology` builds its domain from
`soc_spec["cores"]` only (`topology.py:115-117`). Every `cores[].type` in all
nine SoC files is `cortex-a*` or `cortex-m*` — `cortex-m55` x13, `cortex-a32`
x4, `cortex-a55` x2, `cortex-m33` x2. The fall-through arm's live domain is the
unresolved-sentinel case and nothing else, which is why the defect it carries is
the miss-semantics divergence above rather than a silent mis-build.

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
the `ALP-Bxxx` code, the hint and the `doc:` URL.

**Correcting an attribution made in the first draft of this section.** It cited
the comment at `:437-441` as stating that the message body is passthrough. That
comment belongs to `_BLOCK_SEE_RE` (`:442`) and scopes "taken from the
validator's own output rather than rebuilt from the code" to the **doc URL**
only, for a stated reason: alp-sdk's `_doc_url` honours `ALP_DIAG_BASE_URL`, so
synthesising the path on tan's side would contradict the child whenever that
variable is set. The message body is a different field — the third capture group
of `_RICH_HEADER_RE` (`:408`) — and no comment describes it as passthrough. The
fragility claim survives the correction; the citation did not.

**tan does not merely parse the prose for display — it rebuilds alp-sdk's own
diagnostic document out of the parse.** `_diagnostic_v1_document`
(`tan-cli/python/tan/commands/validate_cmd.py:944`) assembles a `diagnostic-v1`
payload from the regex findings, stamps alp-sdk's `schemaVersion`, and emits it
at `:1154` for `--format diagnostic-v1`; its own docstring says it is "mirroring
`scripts/alp_cli/diagnostic_format.py:to_machine_json`'s shape". So the round
trip today is: alp-sdk builds a structured document, renders it to prose, and tan
regex-parses the prose back into that same schema — while alp-sdk holds an
exporter producing the document directly. That is the sharpest statement of this
problem, and the first draft did not make it.

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
  recovers via `_FAIL_WARN_RE`. Those are the SDK's refusals to build an
  unsupported hardware revision, in Risks below, so "no new schema is
  invented" is false precisely where it matters most. This answers Open
  question 1: **no**, `diagnostic-v1` does not cover them today.
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

## Problem 2 — the class→runtime decision exists three times, and no schema enforces it

### What is there today

Three copies of the same `cortex-a` / `cortex-m` prefix branch, not two:

- `alp-sdk/scripts/alp_orchestrate/topology.py:28-43` —
  `_default_os_from_core_type`, also reached from
  `scripts/alp_orchestrate/loader.py:535-555`.
- `alp-sdk/scripts/alp_orchestrate/topology.py:80-87` — `_runtime_class`, the
  same prefix test a second time in the same file, mapping to `linux` / `rtos`
  instead of to runtime names. Different codomain, identical rule.
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

**What the freshness gate does and does not cover** — the first draft omitted
this, and it cuts both ways. An **alp-sdk-side** edit *is* caught:
`tan-cli/python/tests/gates/test_planner_relocation_freshness.py` pins the
SHA-256 of every upstream `scripts/alp_orchestrate/*.py`, `topology.py`
included, and `dispatch-tan-parity.yml` fires at tan on every alp-sdk push. A
**tan-side** edit alone is caught by nothing: tan's copy lives at
`python/tan/core/os_class.py`, *outside* `tan/planner/`, and it is absent from
`HAND_PORT_SOURCES` (`:993-1006`) — that gate's second audit, the one covering
hand-ported files outside the hashed tree. The exposure is one-directional, in
the direction the gate cannot see. That is tan-cli#279 exactly, the precedent
ADR-0026 cites by name: `PINNED_HASHES` only ever looks inside
`scripts/alp_orchestrate/`.

### Proposed change

**Revised after review — the first draft reached for the wrong prong of its own
test.** It proposed a declared core-class → default-runtime table in
`metadata/`, schema-backed, read by both sides, with a gate asserting exactly
one entry per core class. That is prong (c) — making the *rule* cross — and
prong (b) is answerable first: the SDK can evaluate this rule and ship the
outcome, so nothing needs to cross as a rule at all.

Ship the decided value instead, in the field that already exists for it:

- Populate `os` on all 26 `topology.<core>` entries across the eleven
  `metadata/e1m_modules/*.yaml` presets. `som-preset-v1.schema.json`
  `$defs/topology_entry/properties/os` already declares it — "Default runtime
  for this core.  Customer's board.yaml `cores.<id>.os` overrides." — and
  `scripts/alp_orchestrate/loader.py:555` already prefers it over the rule.
  Every one of the 26 omits it today, so that preference has never once been
  exercised.
- Make `os` `required` in `$defs.topology_entry` (it is `required: []` today),
  so a new SoM family fails validation rather than falling through.
- Keep the class rule as a **producer-side gate only**, asserting the declared
  field agrees with the core class — deliberate redundancy in a gate, which
  this spec's non-goals already permit.
- No new file, no 28th schema, no transport question: tan reads
  `metadata/e1m_modules/` live already.

Against the rule: prong (b) is satisfied, so prongs (a) and (c) never arise.
Consumers read a field; nobody applies a mapping; there is no matching
semantics for a second language to reimplement.

See "What a rule looks like when it becomes data — and why most should not"
above for the general form, the constraints that apply to the genuine prong-(c)
residue, and the live alp-sdk/tan divergence this does **not** fix.

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

**And alp-sdk itself never reads it back.** Every in-repo reference is a
producer or a drift gate: `scripts/gen_error_catalog.py:45` writes it,
`scripts/check_diagnostic_narratives.py:6` and
`tests/scripts/test_gen_error_catalog.py:18` check it against its sources, and
`.github/workflows/pr-generated-files.yml:283,302` plus
`scripts/test-all.sh:964,987,995` regenerate-and-diff it.
`scripts/alp_cli/explain.py`, which would have been the in-repo consumer, no
longer exists. So the heading undersells it: this is not a half-consumed in-repo
artefact but a **pure cross-repo export with exactly one consumer, in another
repo**, and there is no error-catalog schema among the 27 under
`metadata/schemas/`. That is a stronger argument for schema-gating than
"half-consumed" makes — the only thing standing between a shape change and a
broken `tan explain` is a regenerate-and-diff of the producer, which passes by
construction precisely when the producer is what changed.

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

## Problem 4 — `build-plan-v1`'s key names are a three-repo contract with no cross-repo gate

Added after review. This is the same class as Problem 1 — a contract carried by
convention rather than by a mechanism — and it was missing from the first draft.

### What is there today

`metadata/schemas/build-plan-v1.schema.json:8` fixes the top-level `required`
list at eight names:

```json
["schemaVersion", "generatedBy", "boardYaml", "sku", "buildRoot", "slices", "sharedArtefacts", "warnings"]
```

Two other repos restate that list **verbatim**, as source, rather than deriving
it:

- `tan-cli/python/tan/core/build_plan.py:24-27` — `_REQUIRED_TOP`, the same
  eight strings in the same order; `:40` — `_REQUIRED_STR_TOP` repeats four of
  them; `:582` reads `raw["boardYaml"]` positionally into `BuildPlan`.
- `alp-sdk-vscode/src/ideHub/messages.ts` — `interface BuildPlanData`, the same
  eight keys as TypeScript fields (`origin/main` `00d5e6ff` `:262-271`;
  `origin/dev` `6101634f` `:472-481`).

The schema's own description already states the rule, in prose: "camelCase keys
form a CLI-consumer contract, not in-repo YAML. Stability: schemaVersion 1 is
locked with tan-cli; bump it and record the change in CHANGELOG before a
breaking change."

### Why it is fragile

Rename `boardYaml` in a single alp-sdk PR and every gate stays green.
`scripts/check_build_plan.py:40` loads its validator from
`metadata/schemas/build-plan-v1.schema.json` — **the very file the PR just
edited** — so it validates the new plan against the new schema and passes. The
gate cannot see a rename by construction; it has no fixed point to compare
against.

The two consumers then fail differently, and the worse one fails quietly:

- tan raises `build.plan-invalid` — loud, at least.
- alp-sdk-vscode's `BuildPlanData` is a TypeScript *interface*, erased at
  runtime. The field is simply `undefined`. No error, no diagnostic; the panel
  renders a plan with a blank board path.

This is Problem 2's shape one layer up: the rule is stated in a description and
restated in three implementations, and nothing derives it from one place.

### Proposed change

Treat `build-plan-v1`'s key names as ABI, using the mechanism this repo already
runs for the C ABI (`scripts/abi_snapshot.py` + `docs/abi/`, enforced by
`check · generated files in sync`):

- Snapshot the schema-derived key set — top-level plus each `$defs` object — to
  a generated file.
- A gate diffs the live schema's key set against that snapshot. A rename is
  therefore a two-file commit: the schema *and* the snapshot. That makes the
  break an explicit, reviewable decision instead of an invisible one, which is
  the whole property `check_build_plan.py` cannot provide while it reads the
  schema under test.
- State in the same gate's failure text what the schema description already
  says: a key rename is a `schemaVersion` bump, not an edit to version 1.

Against the rule: nothing here crosses the seam as policy — the key names *are*
the seam's vocabulary. It qualifies for the same reason Problem 2 does. The
contract is restated in three places instead of derived from one, and no
mechanism notices when the restatements stop agreeing.

### Explicitly not proposed

Generating tan's `_REQUIRED_TOP` and vscode's `BuildPlanData` from the schema.
That is a codegen step in two more repos to remove a duplication a snapshot gate
already detects, and it would put an alp-sdk build dependency into both
consumers. Detection is the missing property here, not derivation.

## Non-goals

Recorded so they are not re-litigated:

- **Gate allowlists stay in code.** `scripts/check_pin_conflicts.py:47`,
  `scripts/check_doc_drift.py:114`, `scripts/check_doc_links.py:77`. No
  external consumer must agree with a CI suppression list; a central
  exceptions file is a dumping ground.
- **A gate restating a schema rule is not drift.** Deliberate redundancy is
  what a gate is for. Only a *producer* restating a rule is drift. The 67
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
  reached from `loader.py`, which is also where the SDK's refusal to build an
  unsupported hardware revision lives (see below). The change must not alter
  which revisions are refused.
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
2. ~~Where should the core-class → runtime table live — a new `metadata/` file
   with its own schema, or an additive block on an existing one?~~
   **Answered: neither — there should be no table.** The question presupposed
   prong (c). Prong (b) resolves it first: ship the decided `os` value on the
   26 existing `som-preset-v1` `topology.<core>` entries, make the field
   `required`, and keep the class rule as a producer-side gate. See the revised
   Problem 2 proposal and the "What a rule looks like when it becomes data"
   section. One thing stays open behind it, and it is a **sequencing**
   dependency rather than a design one: ADR-0026 amendment sections C and D
   decide whether both repos still need this answer at all, so the schema
   change waits on them. Populating the 26 fields does not.
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
   here; it needs its own decision. Refs
   [#1850](https://github.com/alplabai/alp-sdk/issues/1850), opened for it.
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

Problem 4 and the review corrections above were verified on 2026-08-30 against
`alp-sdk` at `origin/dev` = `ed91fde0`, `tan-cli` at `b9aa697`, and
`alp-sdk-vscode` at `origin/main` = `00d5e6ff` and `origin/dev` = `6101634f`.
The `alp-sdk-vscode` half of Problem 4 was flagged strong-but-unconfirmed in
review because the reviewer's local checkout sat on a feature branch; it is
confirmed here on both of that repo's shared branches, and the consumer is
`src/ideHub/messages.ts`, not `src/ideHub/buildPlanPanel.ts` — that file exists
but contains no `boardYaml` reference.
