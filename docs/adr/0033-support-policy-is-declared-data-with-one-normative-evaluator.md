# 0033. Support policy is declared data with exactly one normative evaluator

Status: Proposed
Date: 2026-08-30
Deciders: alpCaner
Relates to: [0020](0020-sdk-owns-build-execution.md),
[0026](0026-tan-owns-the-planner-outright.md)

## Context

The SDK refuses builds on grounds it presents as facts about silicon. Some of
those grounds are not facts about silicon; they are decisions about what this
SDK supports. Nothing in the tree records which is which, and the two are
enforced by the same code in the same voice.

### The worked example

`board.yaml` may set `cores.<id>.os`. Setting the *other* class's runtime --
`zephyr` on a Cortex-A, `yocto` on a Cortex-M -- is refused at
`scripts/alp_orchestrate/validate.py:270-282`
(`_enforce_os_matches_core_class`), reached from
`scripts/alp_orchestrate/loader.py:910`, with this message:

```text
core '<id>' (<type>): its runtime is determined by the core class
(Cortex-A -> Yocto/Linux, Cortex-M -> Zephyr/RTOS) and is not
selectable. Set os: 'off' to disable it or 'baremetal' for no-OS
firmware -- got os: '<os>'.
```

**"Determined by the core class" is not true as physics.** Upstream Zephyr at
the pinned v4.4.1 ships `arch/arm/core/cortex_a_r/` and
`include/zephyr/arch/arm/cortex_a_r/`. Zephyr runs on Cortex-A. alp-sdk has
*chosen* to carry exactly two OSes and pair one to each core class, so that a
SoM swap within a family keeps the same runtime per core -- ADR
[0011](0011-intra-family-portability.md)'s promise. That is a good decision. It
is a decision. `scripts/alp_orchestrate/topology.py:92` states it as
impossibility -- "A Cortex-A can't run Zephyr" -- and the customer-facing error
inherits that voice.

### Why the misframing has cost

1. **It cannot be found.** No ADR, schema description or doc records that the
   pairing is a support boundary, so a reader looking for "what does this SDK
   support" finds a loader guard phrased as a law of nature.
2. **It is restated three times and has already diverged.**
   `topology.py:28-43` (`_default_os_from_core_type`), `topology.py:80-87`
   (`_runtime_class`, the same prefix test with a `linux`/`rtos` codomain), and
   `tan-cli/python/tan/core/os_class.py`. On an unresolved core type the two
   repos disagree today: alp-sdk `_allowed_os_for_core("")` returns
   `["baremetal", "off"]` and `_default_os_from_core_type(5)` raises
   `AttributeError: 'int' object has no attribute 'lower'`; tan returns `[]` for
   both, guarded at `python/tan/core/os_class.py:127-128` per tan-cli#914 /
   tan-cli#957. tan took that fix and alp-sdk did not
   ([#1852](https://github.com/alplabai/alp-sdk/issues/1852)). Through
   `_cross_class_os` this reaches the refusal: an unclassified core has *both*
   real runtimes refused, and the error reads `(unclassified)`.
3. **The field built to carry the decision is unused.**
   `metadata/schemas/som-preset-v1.schema.json` `$defs/topology_entry/properties/os`
   exists, described as "Default runtime for this core.  Customer's board.yaml
   `cores.<id>.os` overrides", and `loader.py:555` already prefers it:
   `os=str(entry.get("os") or _default_os_from_core_type(soc_core_type))`. All
   **26** per-core `topology` entries across the twelve
   `metadata/e1m_modules/*.yaml` presets omit it, and `$defs.topology_entry` has
   `required: []`, so that preference has never once been exercised. The
   fall-back is the mechanism.
4. **"Policy metadata" names two different things.** `metadata/chips/`,
   `metadata/socs/`, `metadata/e1m_modules/`, `metadata/boards/`,
   `metadata/pinmux/` and `metadata/blocks/` are hardware truth -- they change
   when a datasheet changes. `metadata/emit-registry-v1.json`,
   `metadata/quality-tasks-v1.json`, `metadata/toolchains.json`,
   `metadata/bootstrap.json`, `metadata/library-aliases-v1.json` and
   `metadata/registries/` are decisions -- they change when we decide
   differently. `quality-tasks-v1.json` is already a decision table ("whether
   each is a hard CI gate or informational"). The directory has been carrying
   both under one word.

### The forcing constraint

A second consumer in another language must reach the same verdict. tan is a
second consumer today; alp-sdk-vscode is a third; a Rust-era tool would be a
fourth. Every reimplementation of a rule from prose is a drift site, and
ADR-0026 measures what that costs: 23,886 lines of parity apparatus guarding
7,182 lines of duplicated logic.

## Decision

**alp-sdk declares support policy as data. Exactly one normative evaluator
turns a declared policy into a verdict. Consumers call that evaluator or port
it under audit; they do not re-derive it from prose.**

1. **Declaration is separate from enforcement.** A support boundary is recorded
   as versioned, schema-backed data that says what the SDK supports. Whether a
   given input is refused, and by whom, is a second question with a single
   answer -- clause 2.

2. **One normative evaluator, and it lives in alp-sdk `scripts/`.** It is
   importable with no tan in the process and no Zephyr on the path, because 251
   `CMakeLists.txt` files reach `load_board_yaml` through
   `${ALP_SDK_ROOT}/scripts/alp_project.py --emit zephyr-conf --core <id>` at
   cmake-configure time, and the four `west alp-*` commands
   (`alp-lock`, `alp-migrate`, `alp-quality`, `alp-emit`) are alp-sdk Python.
   An evaluator those paths cannot reach is not an option.

3. **Consumers call it or port it under a hash-pinned audit.** No consumer
   re-derives a declared policy from its description. A port is legitimate where
   a call is not possible -- a different language -- but it is pinned and
   audited the way `HAND_PORT_SOURCES` already pins hand-ports, never left to
   prose agreement.

4. **The evaluator stays small enough that a port is provable, not trusted.**
   Declared policy obeys all of:
   - exact values only -- no prefix, glob or regex in the contract;
   - a closed codomain derived from the one schema that owns those values, not
     re-typed;
   - `required`, `additionalProperties: false`, and a `schemaVersion`;
   - no `default` or catch-all entry -- absence is a validation failure;
   - totality and non-overlap proved by a gate, never by entry ordering;
   - normative miss semantics in the schema description: an unresolvable input
     yields *unresolved*, never a guess.

   These exist so the evaluator is a total lookup of a few dozen lines. An
   evaluator that needs more than that is mechanism, and mechanism stays in code
   per ADR-0026's consolidation rather than becoming a second rules language.

5. **A refusal derived from policy says so.** Where a refusal enforces a support
   decision rather than a hardware limit, the error text and the customer-facing
   docs name it as such and point at where the decision is recorded. Refusals
   that *are* hardware limits keep their current voice; the SDK's revision
   refusals (`SdkRevisionUnknown` / `SdkRevisionNotBuildable` /
   `SdkRevisionUnsupported`) are the model -- they refuse a real
   incompatibility.

6. **Prong (b) before prong (c).** Before a policy is declared for evaluation,
   the prior question is whether the SDK can ship the *outcome* instead, in
   which case no consumer evaluates anything. The class-to-runtime pairing is
   such a case: populate `os` on the 26 preset topology entries, make the field
   `required`, and keep the class rule as a producer-side gate asserting the
   declared value agrees with the core class. Declared-and-evaluated policy is
   for what genuinely depends on consumer-side state the SDK cannot see --
   `_EXECUTION_POLICY`'s `missingTool` is the existing example.

### What this does not decide

It does not decide whether Zephyr-on-Cortex-A becomes supported. That stays a
product decision, and this ADR requires only that the answer be recorded as a
decision rather than asserted as physics.

## Consequences

**Good.** A customer can find out what the SDK supports, and why a refusal
happened, without reading a loader. A second consumer in any language reaches
the same verdict by calling or by an audited port, so the class of drift
ADR-0026 measures does not reopen one layer down. The category confusion under
`metadata/` gets a name, which is the precondition for ever splitting it. Clause
6 means most rules never cross at all.

**west compatibility is preserved, and that is why clause 2 is written the way
it is.** Today the refusal fires on a plain `west build` with tan nowhere in the
loop, via `loader.py:910`. Keeping the evaluator in alp-sdk `scripts/` means
that path keeps its refusal unchanged, all 251 configure-time invocations keep
working, and `west alp-emit` / `alp-lock` / `alp-migrate` / `alp-quality` are
unaffected. The alternative where alp-sdk stops evaluating entirely was rejected
precisely because it would silently drop the refusal for every user building
through west.

**Bad / accepted.** A declared policy is one more contract to version and gate.
Clause 3 puts a call or a pinned port on every consumer, and for tan that means
either depending on an alp-sdk module -- re-introducing a boundary ADR-0020
worked to remove -- or carrying a pinned port. Neither is free, and clause 4's
size limit is what keeps the second option honest. Clause 6's field population
spreads a decision across twelve preset files, so a future change to the policy
edits 26 rows plus its gate rather than one function, and a half-finished edit
validates green with only the gate holding it.

**Risk.** The evaluator grows. Every rule that "almost" fits will argue for one
more matcher, and the end state of that argument is a rules language with two
implementations -- exactly what
`docs/superpowers/specs/2026-08-30-seam-fragility-design.md`'s non-goals reject
as "pure data plus N interpreters". Clause 4 is the brake, and it works only if
a proposed addition that needs a new matcher is read as evidence the rule is
mechanism, not as a gap in the vocabulary.

**Sequencing.** Where a declared policy physically lives is downstream of
ADR-0026's amendment sections C and D, which decide who owns rendered-artefact
bytes and who answers the configure-time CMake call. If those land on "tan owns
the renderers and alp-sdk's emitters die", clause 2's premise changes and this
ADR needs revisiting. Clause 6's field population is safe under every outcome
and does not wait.

## Alternatives considered

- **Leave it as it is: rules in Python, restated per consumer.** Rejected on
  evidence -- the restatements have already diverged
  ([#1852](https://github.com/alplabai/alp-sdk/issues/1852)), in the one
  direction the freshness gate cannot see, because tan's copy sits outside
  `python/tan/planner/` and outside `HAND_PORT_SOURCES`.
- **The SDK declares only, and stops evaluating; the CLI refuses.** Rejected on
  the west measurement above: the refusal would vanish from every plain
  `west build`, which is 251 configure-time call sites with no tan in the
  process. Attractive on paper -- one evaluator, cleanly on the consumer side --
  and untenable given who actually calls the loader.
- **The SDK declares, and both sides run their own decision engine.** Rejected:
  it recreates the parity problem over an untyped rules file instead of Python,
  which the seam spec's non-goals already name as the worse outcome -- "instead
  of two planners you get two interpreters of a rules file, now untyped and
  undebuggable". One normative evaluator is the difference between this ADR and
  that non-goal, and it is the whole difference.
- **Adopt an embeddable policy engine (CUE, Rego, Starlark, WASM).** Rejected
  unchanged from the seam spec's non-goals: disproportionate to a policy surface
  measured in a handful of tables, and it adds a runtime dependency plus
  debugging opacity to a firmware toolchain.
- **Codegen the declared policy into per-language constants.** Rejected for now:
  it keeps one source of truth but adds a build step that itself needs a
  freshness gate. Worth revisiting only if a consumer appears that can neither
  call the evaluator nor carry a pinned port.

## Relationship to earlier ADRs

Supersedes nothing. It **narrows [0026](0026-tan-owns-the-planner-outright.md)
clause 2's justification** rather than its decision: clause 2 keeps `metadata/`,
`metadata/schemas/`, examples and the tooling contracts in alp-sdk because
"Those are hardware truth and stay in the repo that owns the hardware", and that
reason describes only the hardware half of the directory. The decision to keep
them is unaffected; this ADR supplies the missing reason for the other half -- a
declaration is published by the party that owns the thing being declared.

It is compatible with [0020](0020-sdk-owns-build-execution.md): declaring policy
and evaluating it are not command-surface functions, and nothing here re-adds an
SDK command wrapper.
