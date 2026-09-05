# 0026. Tan owns the planner outright; alp-sdk stops being a second producer

Status: Accepted — amended 2026-08-30 (the decision stands; migration step 3's
deletion unit was wrong as scoped, a missing split step is inserted before it,
and clause 4's surviving-oracle and `crates/` bullets are re-homed — see
**Amendment, 2026-08-30**)
Date: 2026-08-08 (Caner)
Deciders: alpCaner (alp-sdk, tan-cli)
Amends: [0020](0020-sdk-owns-build-execution.md) — its 2026-08-03 amendment
clause 3, which keeps alp-sdk's `alp_orchestrate` as "the reference producer"
alongside tan's relocated planner.

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
   - ~~the frozen `crates/` oracle axis, until `crates/` is deleted on its own
     schedule~~ — **struck 2026-08-30 (see §E)**: tan-cli has no top-level
     `crates/` as of `b9aa697`, so this bullet guards nothing;
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

## Amendment, 2026-08-30 (accepted by alpCaner, #1844) — with the migration re-scoped

The decision above is **accepted**. Reading both repos before executing it
found that the Decision and the Migration sections disagree with each other,
and that three things the Migration does not mention have to be decided before
anything is deleted. Nothing in the Decision changes except clause 4's
frozen-`crates/` bullet, struck in place below as already stale. Migration
steps 2-4 are replaced; §G restates the sequence as six steps, of which step 1
is the original step 1 unchanged, repeated because the order is the point.

### A. Migration step 3 is not executable as written

Step 3 says delete `scripts/alp_orchestrate/` "with `git grep` evidence that
nothing references it". That evidence cannot be produced, because the directory
is not the planner. It is simultaneously the planner, the per-slice config
emitters, and the `board.yaml` loader that the rest of the SDK imports as a
library. Measured on `dev` at `00627b88`:

- `scripts/alp_project.py:179-186` imports `load_board_yaml`,
  `emit_system_manifest`, `emit_ipc_contract_h`, `emit_dts_reservations` and
  `emit_os_topology` from `alp_orchestrate`; `:224-230` imports
  `_slice_alp_conf`, `_slice_cmake_args`, `_slice_local_conf`; `:391` imports
  `alp_orchestrate.libraries`.
- `metadata/emit-registry-v1.json` names
  `scripts/alp_orchestrate/{kconfig,manifest,headers,topology,secure,buildplan,kconfig_symbols}.py`
  as the `owner.module` for twelve of the twenty emit modes (the other
eight belong to `scripts/alp_project_emit/*`, `scripts/gen_zephyr_board.py`
and `scripts/alp_template.py`).
- Sixteen gate scripts import it, including
  `scripts/check_zephyr_conf_parity.py`, `scripts/check_system_manifest.py`,
  `scripts/check_emit_registry.py` and `scripts/check_build_plan.py`.

Step 3 is therefore replaced by a **named split**: the plan-*producer* axis
dies; the emitter/loader core survives, or is explicitly moved to tan with the
consequences in §B and §C decided out loud. The record must enumerate, per
emit-registry entry, one of: **dies / stays (extracted to which module) /
moves to tan**. Without that enumeration "stops producing plans" can be
executed as anything from "delete `buildplan.py`" to "delete the tree", and
those two differ by every example build.

### B. The deletion would remove the SDK's refusal to build an unsupported hardware revision, not just a planner

`SdkRevisionUnknown`, `SdkRevisionNotBuildable` and `SdkRevisionUnsupported`
come from `alp_orchestrate.models` (`:30`, `:41`, `:56`). They are raised
inside the directory step 3 proposed to delete, at
`scripts/alp_orchestrate/loader.py:653,660` (unknown), `:702,709` (not
buildable) and `:749` (unsupported), and all three run on every emit because
`load_board_yaml` calls the checks at `.../loader.py:1238-1260`. They are
imported from outside the directory by `scripts/validate_board_yaml.py:21` and
`scripts/gen_catalog.py:93,340,353-359`; sixteen `scripts/check_*.py` import
`alp_orchestrate`, and six of them reach this refusal by calling
`load_board_yaml`. (`scripts/alp_project_loader.py:436-457` raises
`SdkRevisionUnknown` and `SdkRevisionNotBuildable` too, but that is the
secondary `composed-route-table` pad-override path, not the build path.)

That chain is the SDK's refusal to build a hardware revision it does not
support — the mechanism that declines a `status: tbd`
revision rather than producing an image for it.

Two consequences the Migration section does not state:

1. A naive `rm -rf scripts/alp_orchestrate/` deletes that enforcement.
2. `scripts/validate_board_yaml.py` is the script **tan itself spawns** on the
   default `tan validate` path. Measured on `tan-cli` at `b9aa697`, the pin §E
   uses: `python/tan/commands/validate_cmd.py:319` names the script, and the
   `subprocess.run` call is `:1508-1519` (`:1497-1508` in the first draft of
   this amendment started mid-comment and stopped at the opening line); `tan
   diff` does the same at `python/tan/commands/diff_cmd.py:608-628,678`.
   Deleting the directory breaks tan's own default validate path, after tan has
   become the only CLI.

The split in §A must keep the loader and its revision gate.

### C. Who answers the configure-time CMake call

Scaffolded and example `CMakeLists.txt` invoke
`alp_project.py --emit zephyr-conf --core <id>` at cmake-configure time. That
call is load-bearing enough that the template engine rewrites it per-SKU
(`scripts/alp_template.py:1007-1013`, `_substitute_cmake_core`) and carries
dedicated SDK-root discovery for it (`:1186-1210`, the `_ALP_SDK_ROOT_GUESS_RE`
rewrite loop) — both measured at `00627b88`, as in §A, and both since moved by
`722320a1a`; the symbol names are the durable anchors. After this ADR, either a surviving emitter core in
alp-sdk answers it, or it is repointed at `tan` — which makes tan a hard
dependency of **every user project build, including a plain `west build` with
tan nowhere in the loop**. That is materially larger than the Consequences
section's "some alp-sdk CI paths". It also reaches projects already shipped to
customers, which no repo-side change can fix retroactively, so option two needs
a deprecation window rather than a cut. Decide which, here, before step 3.

### D. The renderers are duplicated too, and clause 4 does not police them

The Context table's own measurement — 20 of 21 modules mirrored — includes
`kconfig.py`, `headers.py` and `secure.py`, i.e. the `alp.conf` / sysbuild
renderers, not only the planner. A cross-repo audit on 2026-08-30 confirms the
shape: of the 20 `--emit` modes, **19 are re-implemented in tan** against
alp-sdk's raw metadata and **1 (`scaffold`) is a byte-for-byte vendored
snapshot**; none is consumed as a schema-validated envelope. Retiring only the
plan producer while deleting the parity axis leaves those renderers duplicated
with no gate. The record must name a single owner for rendered-artefact bytes:
either tan's copies become canonical and alp-sdk's emitters die (which implies
§C option two), or alp-sdk's emitters stay canonical and tan consumes
plan-embedded bytes. Left implicit, this recreates the #320 → #485 drift class
one layer down, now undetected.

### E. Oracle and snapshot custody

Clause 4 keeps "`build-plan-v1` shape checks against the frozen oracle", but
alp-sdk's `parity-seam1.yml` diffs the frozen oracle against a **live SDK
emit**. Once the SDK cannot emit, that job is dead by construction. The
surviving checks must move to tan-cli together with the oracle corpus and the
plan-shaped emit-snapshot goldens, and `parity-seam1.yml` and
`dispatch-tan-parity.yml` must be retired in alp-sdk explicitly rather than
left to fail.

Clause 4's second bullet is also already stale: it preserves "the frozen
`crates/` oracle axis, until `crates/` is deleted on its own schedule". As of
`tan-cli` `b9aa697` (2026-08-29) there is no top-level `crates/` directory —
both implementations are Python. That bullet now guards nothing and should be
struck rather than carried forward.

### F. Contract custody and plan provenance

`build-plan-v1.schema.json` stays in alp-sdk (clause 2) while its sole producer
lives in tan-cli. The record must name which gate validates tan's live emit
against the SDK's schema, and the PR ordering when a schema change and its
producer change land in different repos. Separately, plans stamp `sdkVersion`
and `sdkCommit`; when tan is the emitter those fields need a defined meaning
(the consumed metadata checkout's identity, plus a tan version) so a plan
captured on a bench stays attributable to its exact inputs.

### G. Replacement migration sequence

Steps 2-4 above are replaced by the following. The order is load-bearing.

1. **Prove equality at one ref** (unchanged from step 1). Exit condition: tan's
   parity jobs green against a single alp-sdk ref with all three pin sites on
   it. Taken out of order, the deletion becomes a silent behaviour change.
2. **Land the `alp_orchestrate` split** — the step this ADR was missing.
   Separate the planner axis from the emitter/loader core per §A, keeping the
   revision gate of §B. Exit condition: `scripts/alp_project.py` and every
   `check_*.py` import a surviving module, and `--emit build-plan` is the only
   mode owned by the doomed axis. Skipped, step 3 breaks every example
   configure and roughly a dozen gate scripts in one commit.
3. **Repoint alp-sdk's own consumers** — CI workflows, `docs/cli.md`,
   `docs/board-config-features.md`, `docs/heterogeneous-builds.md`, and the
   emit-registry `owner` fields — at tan or at the surviving core. Done before
   step 1, this repoints onto a divergent planner.

   **Exit condition, corrected.** The first draft of this amendment said
   "`git grep alp_orchestrate` returns only history and CHANGELOG". That is not
   reachable and contradicts §A: `alp_orchestrate` is named in 263 tracked files
   on `dev` at `ed91fde0`, including `west.yml`, `include/alp/rpc.h`,
   `src/zephyr/alp_banner.c`, `zephyr/kconfigs/core.kconfig` and
   `meta-alp-sdk/conf/machine/*.conf` — and under §A's split the emitter/loader
   core *survives*, so a live reference is the correct end state, not a leak.
   The exit condition is instead: **no module outside the surviving core
   imports `alp_orchestrate`, and `metadata/emit-registry-v1.json` names no
   doomed-axis module as an `owner`.** Both are checkable; "returns only
   history" is not.
4. **Move oracle and snapshot custody to tan** per §E. Exit condition: tan CI
   runs the shape check with no alp-sdk planner checkout.
5. **Delete the plan producer** in one commit naming this ADR, retiring
   `parity-seam1.yml` and `dispatch-tan-parity.yml` in the same commit.
6. **Delete — not merely disable — the alp-sdk-vs-tan parity axis in tan.**
   Exit condition: the second producer exists in **no alp-sdk release that
   tan's supported-version floor still accepts**. Until tan's minimum supported
   SDK rises past the release that dropped the producer, a user on an old SDK
   with a new tan has two producers again and no gate. That gap is the whole
   difference between disabling the axis and deleting it.

### What this amendment does not change

The Decision, its five clauses, the measurement that motivated them, and the
rejected alternatives all stand. The duplication is real, the parity apparatus
is the dominant cost, and one implementation is the right end state. Reading
tan strengthens the case rather than weakening it: `python/tan/planner/` is a
superset of the SDK planner, and with `crates/` gone there is no
language-boundary argument left for keeping two.
