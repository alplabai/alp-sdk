# 0026. Tan owns the planner outright; alp-sdk stops being a second producer

Status: **Accepted (Decision clauses 1-3) — 2026-08-15; migration step 3 HELD,
see the Amendment below.** Originally Proposed 2026-08-08.
Date: 2026-08-08 (Caner) · 2026-08-15 (acceptance + step-3 hold)
Deciders: alpCaner (alp-sdk, tan-cli)
Amends: [0020](0020-sdk-owns-build-execution.md) — its 2026-08-03 amendment
clause 3, which keeps alp-sdk's `alp_orchestrate` as "the reference producer"
alongside tan's relocated planner.
Applied to a second surface by
[0028](0028-tan-owns-the-model-engine.md), which reuses this ADR's
hardware-truth-vs-consumer principle for the model engine.

## Amendment (2026-08-15 — accepted in direction, HELD in deletion)

The **decision** is accepted: clauses 1-3 stand, and no further investment
should go into keeping two planners honest. The **deletion** — migration step 3,
`Delete scripts/alp_orchestrate/ from alp-sdk in one commit` — is explicitly
**held**, because this ADR's own step 1 precondition is not met today.

Step 1 requires the two sides be "provably equal at one ref" and says **"Do not
start removal against a known-divergent pair — the equal state is what makes the
removal reviewable."** As of 2026-08-15 they are divergent:
**tan-cli#756 is OPEN** (filed 2026-08-15T01:55:20Z), titled *"tan/planner
drift: the dispatched parity suite is red against alp-sdk"* — the same title as
the closed #531. The recurrence chain this ADR records as
#320 → #485 → #531 → #543 therefore continues as **#756**, a fifth instance in
four months. A `fix/756-planner-drift-resync` branch is in flight; a `parity.yml`
run failed at 2026-08-15T15:57:16Z.

Reading the closure of #544/#545/#531/#543/#409 as "unblocked" is the specific
mistake this amendment exists to prevent: **closed issues are not a green parity
run.** Step 3 unblocks only on a green `parity.yml` at the actual merge ref, not
on an issue list.

That #756 exists at all is evidence *for* clauses 1-3, not against them: it is
the fifth time detection caught an instance of a class the apparatus cannot
prevent. Accepting the direction while holding the deletion is the correct
split — it stops new investment in the duplicate immediately, and defers only
the irreversible step.

## Context

ADR-0020's amendment relocated the planner into tan but did not retire the
alp-sdk one. Clause 3 kept `alp_orchestrate --emit build-plan` as "the
reference producer", so today **two independent implementations of the same
planner ship from two repos**, and neither is allowed to be wrong.

Measured on alp-sdk `f30f4d4b` and tan-cli `dev` (`ac7e725`):

| Thing | Size |
|---|---|
| alp-sdk `scripts/alp_orchestrate/` | 21 modules, **7,182 lines** |
| tan `python/tan/planner/` | 31 modules, **12,071 lines** |
| Modules mirrored by basename | **20 of 21** (all but `__main__.py`) |
| Parity apparatus policing the mirror (`tests/parity/`, `python/tests/parity/`) | 47 files, **23,886 lines** |

The last row is the finding. The machinery that exists *solely* to keep the
duplicate honest is **more than three times the size of the logic it
guards**, and roughly twice the size of tan's planner itself. That is not
overhead around a feature; it is the dominant cost of the arrangement.

### The duplication does not hold, empirically

Every alp-sdk planner fix must be hand-ported, and the hand-port is a
separate PR against a separate repo with its own gates. The recurrence record
across tan-cli, oldest first:

- **#274** the emit-parity suite had never been run against `PINNED_SDK_TAG`
  on a clean tree · **#275** `tan/planner/` is a fork, so alp-sdk#1057 can
  never reach `tan flash` · **#279** `zephyr_board.py` is a hand-port outside
  `PINNED_HASHES`, so it drifts silently
- **#313**, **#324** parity fixtures froze the capture host's tool inventory
- **#320** tan emits different Kconfig and accepts a board alp-sdk refuses —
  then **#485** *"#320 recurred"*: `CONFIG_ALP_SDK_CHIP_NONE=y` aborts
  configure on all four Renesas SKUs, and neither freshness gate could block it
- **#425** `PINNED_SDK_TAG` is 7 contract-surface commits stale
- **#492** the planner's own emit correctness — carve-out and partition
  allocators skipping overlap and bounds checks
- **#509** planner-drift detection is *advisory*, and #270 makes it unrunnable
- **#531**, **#543** drift again — #543 records that dev's parity job has
  **failed on every alp-sdk dispatch since 2026-08-07** (the vendored planner
  emits `CONFIG_ALP_SDK_CHIP_DP83825=y` and the SDK does not)
- **#544**, **#545** this session: alp-sdk#1289 and #1331 each needed a
  matching tan port before either fix was actually shipped to users

#320 recurring as #485, and drift standing open again as #531/#543, is the
part that matters. The gates did not prevent the class; they detected
instances of it, sometimes late, sometimes only advisorily.

### The pin makes every port a three-repo-file edit

A port cannot land alone. Because the parity layer compares tan's planner
against a pinned alp-sdk ref, a tan-side port of an alp-sdk fix reads as
*divergence* until the pin moves past that fix — so port and pin bump must be
atomic. And the pin is recorded in **three** places that must agree:
`PINNED_SDK_TAG` (`.github/workflows/parity.yml`), the `sdk_parity` checkout
`ref` (`.github/workflows/ci.yml`), and `PINNED_SDK_COMMIT`
(`python/tests/gates/test_planner_relocation_freshness.py`). Bumping one is a
silent half-move; this session's #544/#545 port bumped exactly one on its
first pass and only caught the other two on a rebase.

Layered on top are `DELIBERATE_EDITS` (with `un_edit` functions, strict — a
healed divergence must force its own entry out), `DELIBERATE_DIVERGENCE` and
`FILE_SET_DIVERGENCE` for the frozen-`crates/` axis, a module-size budget
ratchet, and a re-vendored template tree. Each is individually well-built.
Collectively they are the interest payment on the duplication.

## Decision

**Tan owns the planner. alp-sdk stops producing plans.**

1. `python/tan/planner/` becomes the single implementation. ADR-0020's
   amendment clause 3 is amended: alp-sdk's `alp_orchestrate` is no longer
   "the reference producer".
2. **alp-sdk keeps what it is uniquely authoritative for** — `metadata/`,
   `metadata/schemas/`, examples, and the tooling contracts. Those are
   hardware truth and stay in the repo that owns the hardware. The planner is
   not hardware truth; it is a consumer of it.
3. `build-plan-v1` survives unchanged as the planner/executor seam and the
   public interoperability contract. Retiring the second *producer* does not
   retire the *format*, and `tan build --plan-from <file>` still exercises the
   seam explicitly.
4. The parity apparatus is retired **in proportion to what it still guards**,
   not wholesale. The alp-sdk-vs-tan planner axis goes away with the second
   producer. What must NOT be dropped in the same motion:
   - the **metadata/schema** contract checks — tan still consumes alp-sdk
     metadata, so that seam is real and needs a gate;
   - the frozen `crates/` oracle axis, until `crates/` is deleted on its own
     schedule (tan-cli#409 already tracks freezing the live-only cases first);
   - `build-plan-v1` shape checks against the frozen oracle.
5. The three pin sites and `DELIBERATE_EDITS` disappear with the axis they
   serve.

### Migration, in an order that is safe to stop at any point

1. Land the outstanding ports so the two sides are provably equal at one ref:
   #544 and #545, then close the live drift in #531/#543. **Do not start
   removal against a known-divergent pair** — the equal state is what makes
   the removal reviewable.
2. Repoint alp-sdk's own consumers (CI, docs, `scripts/`) at tan.
3. Delete `scripts/alp_orchestrate/` from alp-sdk in one commit that names
   this ADR, with `git grep` evidence that nothing references it.
4. Retire only the now-dead parity axis, keeping clause 4's exclusions.

## Consequences

**Good.** One planner, so a fix ships once and is shipped everywhere by
construction rather than by a gate noticing later. ~7,182 lines of duplicated
logic and the majority of 23,886 lines of parity machinery are deleted, not
maintained. The #320 → #485 → #531 → #543 recurrence class stops existing:
there is no second implementation to drift. A hardware fix reaches users in
one PR instead of two coupled ones across two repos.

**Bad / accepted.** alp-sdk can no longer emit a plan standalone — anything
that wants one needs tan, which makes tan a hard dependency of some alp-sdk CI
paths that are self-contained today. The frozen `crates/` oracle stops having
a live second opinion to check against, so the surviving oracle checks carry
more weight and must not be trimmed at the same time (clause 4). And the
parity suite has repeatedly caught real defects (#485, #543): removing the
axis removes that detection, which is only acceptable *because* the thing it
detects can no longer occur, not because the detection was worthless.

**Risk.** The removal commit is large and touches CI. Step 1's "provably
equal at one ref" precondition is what keeps it reviewable; skipping it
converts this from a deletion into a silent behaviour change.

## Alternatives considered

- **Keep both, harden the gates further.** Rejected on the measurement: the
  gates are already 3× the guarded logic and #320 still recurred as #485, and
  #531/#543 are open now. More apparatus buys detection, never prevention.
- **Generate tan's planner from alp-sdk's at build time.** Rejected: it keeps
  two sources of truth and adds a codegen step, trading a hand-port for a
  generator that itself needs a parity gate.
- **Move the planner back into alp-sdk and have tan call out to it.**
  Rejected: it re-breaks ADR-0020's single-executor decision and reintroduces
  the subprocess boundary the Python port removed, for a repo-layout
  preference rather than a technical gain.
