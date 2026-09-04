# 0020. The SDK plans; a standalone `tan` CLI is the whole command surface (three repos, one executor)

Status: Accepted — amended 2026-08-03 for the Python Tan port, 2026-08-12,
2026-08-26; the original Rust/plans-only mechanism below remains the
historical Phase 1/4 record.
Date: 2026-07-18 (Caner) · 2026-07-20 (Hakan co-sign, this commit)
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode)
Supersedes: [0014](0014-build-plan-emit-cli-contract.md) — its
mechanism clause **and** its 84-87 consequence (`west alp-build` stays native).
Pairs with RFC #837 (`alp` → `tan`).

## Amendment (2026-08-26 — Amendment 7's cross-repo-trigger condition is now met)

Corrects Amendment 7's third bullet (below, dated 2026-07-28: "**Cross-repo
trigger — NOT done, and it is the one real gap.**") and the index row's
matching present-tense clause in `docs/adr/README.md`. Both were accurate when
written; the trigger shipped the same day, seven hours later.

`.github/workflows/dispatch-tan-parity.yml` is the sender half: it fires a
`repository_dispatch` (`event_type=alp-sdk-planner-change`) at
`alplabai/tan-cli` on every push to `dev`/`main`, using a token minted at
runtime from the org's GitHub App -- one of the two credential types
Amendment 7's "cannot be built from either repo's CI without a PAT/App
secret" already named. Item 7 was right about the credential; it was wrong
that obtaining one made this a **maintainer action** -- the App token is
minted automatically at workflow runtime, not provisioned by hand. Landed
`6595d2a4` (2026-07-28, same day as the Amendment-7 commit `0c5cf608`). The
receiver half is live in `tan-cli`: `parity.yml:69` and
`planner-resync.yml:101`, both `types: [alp-sdk-planner-change]`.

Re-running Amendment 7's grep today (`grep -rn repository_dispatch
.github/workflows/*.yml`) no longer returns only the `pr-bitbake.yml` comment
it cited — it also matches `dispatch-tan-parity.yml`, which carries the real
trigger.

One residual gap the workflow's own header documents and this correction
does not close: `repository_dispatch` does not carry the sender's SHA into
the receiver's run ref, so nothing in this repo can filter tan-cli's run list
down to "the run this dispatch caused" versus a concurrent sender — recency
plus `event=repository_dispatch` is the only correlation available
(`dispatch-tan-parity.yml`'s own in-file NOTE, near its `dispatch-confirm.sh`
step).

## Amendment (2026-08-26 — PR alplabai/tan-cli#530 shipped in tan v0.6.0-rc1)

Corrects the 2026-08-12 Amendment's closing paragraph (below): "**Not yet in
a released `tan`.**" no longer holds. `alplabai/tan-cli#530` shipped in tan
`v0.6.0-rc1` (tagged 2026-08-14; `TAN_VERSION` at `python/tan/version.py:49`),
carried forward unchanged into the final `v0.6.0` (tagged 2026-08-24) — both
are real, existing tags, not `dev`-branch-only. `python/tan/core/tool_lookup.py`'s
`resolve_tool()` and `python/tan/commands/build/execute.py`'s `_spawn_step()`
(the "`program` is always the RESOLVED absolute path (tan-cli#510)" docstring
the paragraph below quotes) are both present at that tag.
`build-plan-v1.schema.json:50`'s `missingTool` description, which cross-references
this Amendment by date, now describes released behaviour rather than a
both-ways hedge. The Amendment's five numbered technical claims and the
Consequence paragraph are otherwise unaffected and stand.

## Amendment (2026-08-26 — `crates/` retired, not just frozen)

The "Amendment (2026-08-03)" section below says "the old `crates/` tree is
frozen at v0.4.1 as a behaviour oracle, not the active implementation." That
was accurate on 2026-08-03; it stopped being accurate on 2026-08-10, when
`tan-cli`'s `2883cdf4` ("retire the Rust oracle -- delete crates/ and the
oracle-parity suite (#269) (#601)") deleted the tree outright —
`git ls-tree origin/dev -- crates` on `tan-cli` is now empty. The paragraph
below stays as the dated record of the frozen-not-deleted interim state;
read "frozen" there as superseded by deletion, not as the current state.

## Amendment (2026-08-12 — what the Security clause's "never PATH" means)

The *Consequences* → **Security** paragraph below reads "the executor confines
writes under `buildRoot`, resolves tools/interpreters by explicit path (never
PATH)", and *The plan contract must be complete before the SDK stops executing*
carries the short form of the same claim ("resolves tools by explicit path").
Both were written before an executor implemented them, and "never PATH" reads as
"PATH is never consulted" — which is not what `tan` does, and was never the
property the clause was protecting. This amendment states the property precisely;
the paragraphs below stay as the dated record.

**What is true**, as implemented by `alplabai/tan-cli#510` (PR
`alplabai/tan-cli#530`, merged to `tan-cli`'s `dev` as `50788ecd`, milestone
v0.6.0):

1. `command.tool` stays an **identity** in the plan — never a path, never a
   token. Resolution is executor-owned. (alp-sdk#1286 asked the planner to emit a
   resolved `tool` and was refused; alp-sdk#1291 put that in `command.tool`'s own
   schema description.)
2. The executor resolves that identity to a **concrete filesystem path** with one
   hardened lookup (`resolve_tool()` in `tan/core/tool_lookup.py`) and spawns **that
   path**, never the bare name. On the build path that is
   `_spawn_step()` in `execute.py`, whose own docstring says "`program` is always the
   RESOLVED absolute path (tan-cli#510), never a bare identity"; on the flash
   path it is passed as `subprocess` `executable=`.
3. **PATH is still the search input** for a bare identity — POSIX via
   `os.get_exec_path` + `shutil.which`, Windows via a hand-rolled `%PATH%` walk
   that is deliberately not `shutil.which`. What "never PATH" forbids is handing
   the bare name to the *platform's own* resolver at spawn time: `CreateProcess`
   with `lpApplicationName=NULL` searches the parent process's current directory
   ahead of `%PATH%`, so a checkout carrying its own `west.exe`/`openocd.exe` at
   its root could otherwise be spawned in place of the real tool.
4. The PATH searched is the **slice's own fully assembled `env`** (post
   `envAppendPath` / venv), not the executor process's environment.
5. An **absolute** `command.tool` is answered by existence alone and spawned
   verbatim — no PATH walk happens for it at all.

**Consequence for `executionPolicy.missingTool`.** It fires when `command.tool`
**cannot be resolved to an executable**, which is a superset of "not found on
PATH": an absolute `tool` that does not exist reaches it without PATH being
consulted, and on Windows a `%PATH%` file whose name carries no `%PATHEXT%`
extension is deliberately never selected by the hardened walk.
`metadata/schemas/build-plan-v1.schema.json`'s `missingTool` description is
re-worded to match (issue #1314). alp-sdk#1291 left the old PATH-pinned wording
in place deliberately, on the reasoning that the ADR was the wrong half while
tan-cli#510 was still open; #510 is now closed by #530, so the schema was the
stale half.

**Not yet in a released `tan`.** PR #530 is on `tan-cli`'s `dev` branch. The
latest `tan` release is **v0.5.1** (2026-08-05), which still spawns the bare
identity. The re-worded schema description is chosen to be true of both: "cannot
be resolved to an executable" holds whether the executor resolves-then-spawns or
hands the name to the platform resolver.

## Amendment (2026-08-03 — Python Tan relocates the planner)

The command-surface and single-executor decision stands, but the repository
boundary changed during the Python port:

1. The current Tan development implementation is Python. Until v0.5 is cut,
   alp-sdk `dev` installs `tan-cli/dev` with Python 3.12+; from v0.5, release
   archives are PyInstaller freezes. The old `crates/` tree is frozen at v0.4.1
   as a behaviour oracle, not the active implementation. **`crates/` was
   subsequently deleted outright — see "Amendment (2026-08-26)" above.**
2. Normal `tan build` no longer spawns alp-sdk's planner. Tan owns a relocated
   in-process planner and executor that read alp-sdk metadata, schemas, examples,
   and selected tooling contracts.
3. The `build-plan-v1` shape remains the internal planner/executor seam and a
   public parity/interoperability contract. alp-sdk's
   `alp_orchestrate --emit build-plan` and other emitters remain the reference
   producer; `tan build --plan-from <file>` can exercise that seam explicitly.
4. Most commands are now native Python implementations. Only `migrate`, `lock`,
   and `quality` forward to the surviving west extensions.
5. During the port, `tan sdk list/current` work but `sdk install/switch` refuse
   with `sdk.not-ported`; select an SDK with `--sdk-root`, `.alp/sdk-path`, a
   sibling checkout, or `ALP_SDK_ROOT`.

This amendment supersedes current-state statements below that call alp-sdk
"plans-only", describe Rust as the executor, or say Tan consumes the SDK plan as
its sole input. Those statements remain as the dated migration record.

> **Implemented.** The SDK-side executor, `west alp-build`, and every SDK-side
> user command are retired; the whole command surface now lives in the
> standalone, public **`tan` CLI** ([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)).
> alp-sdk is plans-only: `alp_orchestrate --emit build-plan` / `--emit
> system-manifest` are the sole outputs `tan` consumes (`tan build --native`,
> `tan build` / `flash` / `image` / `size` / `renode` / `clean` for
> hardware/manifest-driven runs). Phase 2 (the extraction) was Hakan's repos
> and his to drive; alp-sdk owned Phases 1 and 4. See *Migration* below for
> the phase history and *The one thing that must hold* for the completeness
> gate this satisfied before Phase 4 deleted the SDK-side executor.

## Amendment (2026-07-20 — Hakan co-sign + corrections)

Hakan ratifies the **direction** (end-state B) and the alp-sdk-side Phase 1/4
code. Three points below correct or condition the record; the release train is
blocked until the remediation is met. Tracked in #855.

1. **The completeness gate this ADR mandates is not yet in place.** *The one thing
   that must hold* + *Cross-repo oracle trigger* require an automatic
   `repository_dispatch` from alp-sdk CI into `tan`'s build-validation on every
   planner change, plus the two-seam comparator, **before** the irreversible
   Phase 4. That trigger does not exist in `.github/workflows` yet — Phase 4 code
   (fan_out deletion, #848) landed ahead of it. This is recoverable, not a
   rollback: `df312cec^` (`97ad481b`) still carries both `fan_out` and the Phase-1
   fields, so the oracle is reconstructed retroactively. **Remediation (blocks any
   release/tag): freeze that oracle, stand up the two-seam gate + the cross-repo
   trigger, then tag.** (Verified: the only `97ad481b`↔`df312cec` emit delta is
   `debug.probe` `"openocd"→null`, hand-reviewed.)

2. **"No `alp`-named command survives" is narrower than shipped.** Retired: the six
   build verbs + `fan_out`. Surviving on `dev` **by design**: the `--emit`
   planner surface, `west alp-migrate/alp-lock/alp-quality/alp-emit`, and the
   Python `alp` console script with 11 non-build verbs
   (`generate/validate/init/doctor/run/model/monitor/new_som/faultdecode/explain/emit`),
   which `tan` forwards to. `docs/cli.md` already documents this real end-state;
   §Decision-1/§Open-Q-4 overstate it. No surviving SDK verb shells out to `tan`
   (dependency stays one-way, tan→SDK).

3. **Contract fix owed to v0.12 (schema-`required` at unchanged version).** `#847`
   made `executionPolicy` `required` in `build-plan-v1.schema.json` while
   `schemaVersion` stayed `const: 1` — a breaking shape change without a bump.
   Since the consumer pins `schemaVersion == 1`, do **not** bump to 2 (it would
   strand `tan`); instead revert the two fields to **optional**, keep the emitter
   always emitting explicit values (strict-producer / tolerant-consumer). `tan`
   already honors + defaults both. This is the version-skew guard (§Decision-5)
   applied correctly.

4. **Second hand-reviewed seam-1 delta — the #863/#871 per-core config
   wiring, superseded 2026-07-22 by a seam-1 SCOPE retune (#874 follow-up;
   pending Hakan's re-ratification, see below).** The planner wires each
   core's `alp.conf` into the plan: a `-DEXTRA_CONF_FILE=<build/<core>-
   zephyr/alp.conf>` arg on every **non-sysbuild** Zephyr slice, plus the
   `_emit_library_hw_backends` (`# ...lib.loader`) HW-accelerator Kconfig
   block folded into each Zephyr slice's `configArtefacts`. Both post-date
   the frozen `97ad481b` oracle. The command-arg addition is still a real
   plan-SHAPE fact the comparator normalizes away (`_strip_863_extra_conf_
   file_arg`, scoped to non-sysbuild slices — a sysbuild slice wrongly
   gaining the arg still fails, see below). The `lib.loader` Kconfig block
   was originally handled the same probe-delta way (a bespoke strip inside
   the compared config-artefact CONTENT); that strip is now moot and
   deleted, because **seam-1 no longer compares config-artefact content at
   all** — see the retune paragraph below. **Sysbuild slices deliberately
   carry NO `-DEXTRA_CONF_FILE`** (Option A): a bare top-level
   `-DEXTRA_CONF_FILE` under `--sysbuild` lands on the sysbuild image, not
   the application image, so it would silently drop the per-core config on
   `boot:`/OTA projects; those slices get the per-core `alp.conf` via the
   app's `--core`-scoped `CMakeLists.txt` bridge (#870), and a plan-native
   per-image sysbuild wiring stays #866. The seam-2 real-build proof of the
   sysbuild path (`iot-fleet-ota`) is the one deferred box on #871.

   **Seam-1 scope retune (2026-07-22, #874 follow-up — narrows this
   co-signed gate's contract, pending Hakan's re-ratification the same way
   as this Amendment):** every intentional emitter content change (a
   Kconfig dependency-gating fix, a new peripheral default) forced another
   bespoke content strip into `normalize_plan`, eroding seam-1 into a
   content-diff gate instead of the plan-SHAPE gate this Amendment
   describes. Seam-1 (`tests/parity/seam1_field_diff.py`, and its tan-cli
   vendored twin — keep in lockstep) now verifies **command, env, appDir,
   skip/fail-decision SHAPE, and `debug.probe` only**; every artefact's
   materialised `contents` (`configArtefacts[*].contents` /
   `sharedArtefacts[*].contents`) is dropped before the diff runs
   (`_drop_artefact_contents`), keeping only its `path` in the shape check
   (an artefact appearing/vanishing/moving still fails the gate). Content
   parity moved to `tests/fixtures/emit-snapshots/*.{build-plan,
   zephyr-conf}.snap` (`scripts/check_emit_snapshots.py`) — confirmed
   complete over every oracle fixture at retune time, see
   `tests/parity/README.md`'s coverage table — and, eventually, seam-2's
   real build.

5. **Hermetic build plans (#865): the emit is now `planPathMode: tokened`.**
   Every path this plan bakes anchored on the emitting checkout or project
   (`env.ALP_SDK_ROOT`, `envAppendPath`, `slices[].appDir`, and each Zephyr/
   baremetal command's app-dir/`-DPython3_EXECUTABLE=`/`-DSB_CONF_FILE=`/
   `-DEXTRA_CONF_FILE=` args) is a literal `${SDK_ROOT}`/`${PROJECT_ROOT}`/
   `${PYTHON}` token instead of an absolute path, so a plan produced on one
   machine/checkout materialises correctly on another instead of silently
   pinning the wrong tree. Consumer: `tan-cli` #24 (already merged), which
   substitutes the tokens and requires `planPathMode: "tokened"`. The seam-1
   comparator reconciles this like the #863/#871 additions above: a tokened
   live plan maps `${SDK_ROOT}` -> `__SDKROOT__` and `${PROJECT_ROOT}` ->
   `__SDKROOT__/<boardYaml's own directory>` (the harness fixtures live
   under the SDK root, and `boardYaml` itself is deliberately left
   un-tokened as that anchor) before diffing against the frozen absolute-path
   `97ad481b` oracle.

6. **(2026-07-27) Point 2 above is itself now stale, on two counts — corrected
   here rather than edited in place, so the record shows what changed.**
   First, "the Python `alp` console script" was never the right name for what
   survives: `pyproject.toml`'s `[project.scripts]` registers only
   `alp-mcp` — its own comment says so ("No `alp` console-script:
   ... invoked as `python -m alp_cli <sub>` — never as a user-installed
   `alp` binary"). What survives is the `alp_cli` **package**, run as
   `python -m alp_cli <sub>`; `docs/cli.md` has always described it this
   way, so point 2's phrasing drifted from the doc it cited as authoritative
   even at the time it was written. Second, `emit` is no longer one of the
   verbs `tan` forwards to: `tan` 0.3.1 has no `emit` subcommand at all
   (`tan emit` → `error: unrecognized subcommand 'emit'`) — it was retired
   and replaced by the narrower, differently-shaped
   `tan generate --target` `<mode>` (6 of
   `metadata/emit-registry-v1.json`'s 20 registered modes,
   fixed output paths, no `--output`/`--core`/`--template`/`--sku`). Point
   2's "11 non-build verbs" all still survive in the `alp_cli` package --
   `scripts/alp_cli/main.py`'s 11 `cli.add_command(...)` registrations
   (`doctor`, `emit`, `explain`, `faultdecode`, `generate`, `init`,
   `model`, `monitor`, `new_som`, `run`, `validate`; NOT a raw
   `ls scripts/alp_cli/*.py`, which also lists 8 non-verb modules --
   `__init__.py`, `__main__.py`, `main.py` itself, `_workspace.py`,
   `diagnostic.py`, `diagnostic_format.py`, `validator.py`,
   `yaml_pos.py`); what narrowed is the *forwarded* set.

   `tan` forwards 9 of them
   (`generate/validate/init/run/model/monitor/new-som/faultdecode/explain`),
   never `emit` (retired, per above) and never `doctor` (native Rust —
   `docs/cli.md`'s forwarding table already reads
   `tan doctor  native-host · none`).

   This landed alongside the full 15-mode classification `docs/cli.md`'s
   "Six modes, no front door: which ones are gaps?" now carries: of the
   nine `alp_project.py`/orchestrator modes with no `tan` front door,
   three (`system-manifest`, `dts-reservations`, `ipc-contract-h`) are
   deliberately orchestrator-internal (`west alp-emit` already reaches
   them, and `tan` consumes `system-manifest` indirectly); `scaffold` is
   consumed by `tan` already, just not live (vendored into the binary at
   release time, `tan-cli`#14 tracks finishing the vendored set);
   `composed-route-table` is an intentional maintainer-only pad-route
   regression/demonstrator tool with no product consumer; and the
   remaining four (`hw-info-h`, `west-libraries`, `os-topology`,
   `zephyr-board`) were real gaps with no design reason for the absence,
   filed as `tan-cli`#113–#116 -- all four CLOSED, all four targets now
   present in Python Tan's `--target` set
   (`python/tan/commands/generate_cmd.py:139-160`), so this row of
   `docs/cli.md`'s gap table is stale and should be dropped.

7. **(2026-07-28) Remediation status — point 1's gate is met on two of its
   three conditions, and its "blocks any release/tag" clause was overtaken by
   events.** Point 1 conditioned any release on *"freeze that oracle, stand up
   the two-seam gate + the cross-repo trigger, then tag."* Re-verified against
   the code rather than assumed:

   - **Oracle frozen — done.** Six fixtures under `tests/parity/oracle/`,
     provenance and every hand-reviewed field correction recorded in
     `ORACLE-PROVENANCE.txt` (the #862 `-DSB_CONF_FILE` anchor, and the #999
     `multicore_rpmsg-imx93` slices[1] `command`→`null` + `board-tree-missing`
     correction).
   - **Two-seam gate — done.** Seam 1 runs on both sides
     (`.github/workflows/parity-seam1.yml` here; `seam1 -- plan-shape parity`
     in tan against `PINNED_SDK_TAG`). Seam 2 is implemented and green —
     `tan-cli`#146, merged `33fe9f5b` — materialise → a real cross-compile
     *through* `tan` → ARM ELF → Renode boot, all asserted. **Scope caveat,
     stated rather than glossed:** seam 2 covers ONE representative slice
     (AEN801 M55-HP, `hello-world`), not all six oracle boards, and no
     `--sysbuild` case; the `iot-fleet-ota` sysbuild proof remains the one
     deferred box on #871.
   - **Cross-repo trigger — NOT done, and it is the one real gap.** No
     `repository_dispatch` from alp-sdk into `tan` exists anywhere in
     `.github/workflows` (grep-verified; the only occurrence of the keyword is
     an unrelated comment in `pr-bitbake.yml` about alp-sdk-internal). It
     cannot be built from either repo's CI without a PAT/App secret, so it is a
     **maintainer action**, not an implementation task. The consequence is
     concrete: `PINNED_SDK_TAG` is bumped by hand and therefore **rots
     silently** — both seams stay green against an alp-sdk that no longer
     reflects this repo's planner. The token-free half is closed by
     `tan-cli`#153, a warn-only freshness check that reports how far the pin is
     behind counting only the contract surface (`scripts/alp_orchestrate`,
     `metadata`, `tests/parity`), so a docs-only week does not cry wolf. That
     makes the missing dispatch **visible instead of silent; it does not
     replace it.**

   **The tag gate did not hold.** Point 1 said the remediation blocks any
   release; `v0.12.0` and then `v0.13.0` were both tagged with the trigger
   still absent, `tan` released `v0.4.0` (validated by a `v0.4.0-rc1` first),
   and the extension pinned `SUPPORTED_CLI_VERSION` to `0.4.0`
   (`alp-sdk-vscode`#385). Recording that plainly: the release train ran on two
   of the three conditions. Whether that was the right call is not re-litigated
   here — what would be wrong is leaving a co-signed gate on the record as
   though it had been satisfied.

   Point 3's contract fix is **also done** and needs no further action:
   `executionPolicy` is absent from `build-plan-v1.schema.json`'s top-level
   `required` and its description reads *"Additive, schemaVersion 1"* — the
   strict-producer / tolerant-consumer shape point 3 asked for. Its companion
   clause, *"reconcile the stale #839 `schemaVersion: 2` fixture"*, is
   satisfied in a way that must **not** be "fixed": the two surviving
   `"schemaVersion": 2` occurrences are deliberate **negative tests**
   (`tests/scripts/test_check_build_plan.py:90`, carrying the comment
   `# locked const -- any other value must fail`, and
   `tests/scripts/test_build_plan_schema.py:520`), asserting that a future
   breaking bump the consumer has not been told about fails rather than
   silently validating. Deleting them would delete a real assertion.

   Two seam-1 weaknesses found while verifying the above, both in the
   comparator rather than the contract, and both worth recording because a gate
   that cannot fail is indistinguishable from one that passes:

   - **Point 4's "keep in lockstep" had silently failed, and a tolerance grew
     to cover for it.** When #999 landed here, this repo fixed it the way the
     oracle's own discipline prescribes — by re-freezing
     `multicore_rpmsg-imx93.build-plan.json` (`slices[1]` `command`→`null` plus
     a `board-tree-missing` warning) and recording the hand-review in
     `ORACLE-PROVENANCE.txt`, leaving `diff_plans` untouched. tan's **vendored
     copy of that fixture was never re-synced.** Its live diff therefore showed
     a command vanishing, and a `_ALLOWED_COMMAND_TO_NULL` tolerance was added
     on that side instead — a permanent comparator allowance standing in for a
     one-file sync. Five of the six fixtures were byte-identical across the two
     repos; only that one had drifted, far enough that it still baked the
     absolute scratch path of the machine it was first frozen on.
   - **That tolerance was itself uncoupled.** `is_command_dropped` and
     `is_warning_added` were independent `or` branches, so a `command`→`null`
     delta passed **whether or not** an accompanying warning appeared — despite
     the code's own comment calling the paired warning *"the guard's whole
     point — to say why."* A slice could lose its command silently and seam 1
     stay green.
   - `tests/parity/test_seam1_field_diff.py` — the comparator's own
     negative-matrix suite, on **both** sides — was executed by no workflow:
     tan's `parity.yml` ran the parity scripts directly with no pytest step,
     and this repo's `scripts/test-all.sh` pytest stage covers `tests/scripts/`
     only. A gate that cannot fail is indistinguishable from one that passes,
     and this one guarded the comparator that judges every planner change.

   Resolved in `tan-cli`#156: the fixture is re-vendored from this repo at
   `PINNED_SDK_TAG` (all six now verified byte-identical), the tolerance and
   its three helpers are deleted so `debug.probe` is once again the only
   allowed delta, and the negative-matrix suite runs in CI ahead of the live
   comparator. The lesson is worth keeping even after the fix: **a vendored
   oracle that drifts does not announce itself — it shows up as a plausible
   diff, and the cheap response is to widen the comparator rather than re-sync
   the fixture.** Point 4's lockstep note is a real constraint, not a
   pleasantry; when the two copies disagree, re-vendor and record it, and treat
   a new comparator allowance as evidence the sync was skipped.

8. **(2026-08-26) Point 6's "11 non-build verbs all still survive in the
   `alp_cli` package" is now false — alp-sdk#1367/#1368 deleted the rest of
   the command-line wrappers.** `scripts/alp_cli/main.py`, `__main__.py`,
   `validate.py`, `doctor.py`, `emit.py`, `faultdecode.py`, `generate.py`,
   `init.py`, `model.py`, `monitor.py`, `new_som.py`, and `run.py` are gone;
   there is no `python -m alp_cli <verb>` front door left to run at all, and
   every one of the 11 verbs point 6 enumerated is now a native `tan`
   implementation only (all present in `tan-cli` `v0.6.0`). Point 6's "8
   non-verb modules" list is also stale: `__main__.py` and `main.py` are
   among the deleted, not the survivors. What remains under
   `scripts/alp_cli/` is six library modules with real non-CLI callers
   (`__init__.py`, `diagnostic.py`, `diagnostic_format.py`, `validator.py`,
   `_workspace.py`, `yaml_pos.py`) — not a command surface of any kind.
   alp-sdk's own command surface is now, in full, what §Decision-2 always
   named as the end state: zero user commands.

## Context

### The problem

Two code paths turn a `board.yaml` into per-core builds: `Orchestrator.fan_out()`
(SDK Python, drives `west alp-build`) and the Rust CLI's executor (in
alp-sdk-vscode) that consumes `--emit build-plan`. RFC #843. The drift is real,
not hypothetical: the plan's per-slice `env` carried only
`{"ALP_SDK_ROOT": ...}` (`buildplan.py:384`), so the Rust executor hand-ported
`ZEPHYR_BASE` / `EXTRA_ZEPHYR_MODULES` + the skip-vs-fail policy
(`orchestrator.py:300-304`). ADR 0014's "cannot drift by construction" holds for
artefact *bytes*, not *execution semantics*, and it leaked within ~6 weeks while
both executors existed — invisible until a bench failure.

### The decision, and why B over A

- **A** — the executor stays in the SDK (Python `fan_out`); a thin manager
  dispatches to it. Keeps the SDK standalone-buildable + an in-repo build oracle;
  costs release-train coupling.
- **B** (this ADR) — the executor and the whole user command surface live above
  the SDK, in a standalone `tan` CLI; the SDK is a plans-only backend.

**B is chosen** (held up under adversarial review): Hakan's CLI-side ADR already
makes the native CLI the single executing surface; the "not standalone-buildable"
objection is weak (building always needs both an SDK checkout and the CLI, which
is a normal setup); Rust owns process/cancel/streaming natively; and it fully
decouples release trains. Honest costs — no in-repo build oracle, and it reverses
0014:84-87 — are accepted deliberately (recorded under *Consequences*). A is kept
under *Alternatives* as the reversible fallback until Phase 4.

## Decision

**alp-sdk plans; a standalone `tan` CLI is the entire user command surface and
the sole executor; the IDE shells `tan`. Three repos.** No `alp`-named command
survives anywhere (RFC #837).

1. **alp-sdk = planner + libraries + Python backend, ZERO user commands.** It
   ships the C/HAL/metadata and the Python planner/validator/generator logic
   that `tan` invokes (`--emit`, `validate_board_yaml.py`, `alp_orchestrate`).
   **Retired:** `Orchestrator.fan_out()` / `_dispatch_slice()`, the `west alp-*`
   extensions (build/flash/image/size/renode/clean), and the `alp` console
   script — all in Phase 4, not before the contract is complete.

2. **`tan` — a NEW standalone repo (Rust), the whole command surface.** Extracted
   out of alp-sdk-vscode's `cli-rs/` and grown to own **`tan build / flash / image / size / renode / clean / sdk / doctor / validate / …`**. It is the sole
   executor (runs `west` / `bitbake` / `cmake` per slice), owns skip-vs-fail, env
   application, scheduling, cancellation, progress UX, SDK lifecycle, **and both
   ends of the manifest** — it writes `system-manifest.yaml` + `.alp-build-state.json`
   (seeded by the planner's `--emit system-manifest`) and its own flash/size/image
   read them back. Independently versioned + distributed (npm shim / prebuilt).
   **This is what a standalone SDK user installs — without the VS Code
   extension.**

3. **alp-sdk-vscode = the extension only.** Thin; shells `tan` by explicit binary
   path. Bundles or downloads the `tan` binary **with pinned-hash / signature
   verification** (it executes arbitrary build commands — the download is the
   softest link otherwise).

   **One-way dependency: extension → tan → alp-sdk.** Installing `tan` never
   drags in the extension.

4. **Two seams, both resolved inside `tan`.** IN: the plan JSON
   (`--emit build-plan`) — the executor's only input. OUT: the manifest + state
   files — consumed by `tan`'s own flash/size/image. Because `tan` owns the whole
   surface, there is **no cross-repo output contract** — the manifest is internal
   to `tan`, seeded by the SDK emit it also consumes.

5. **Version-skew guard (prevents #843 via skew).** The contract grows additively
   (`schemaVersion 1`), so an *older* `tan` release could ignore a new field and
   silently fall back to hand-porting — re-creating the drift. Mitigation: the
   plan may mark a key **required-for-execution**; `tan` **fails loudly on an
   unknown required key** rather than falling back. (Not "drift cannot fork" — it
   can, via skew; this is the guard against it.)

The planner is **not** rewritten to Rust (illusory no-Python prize; `west` /
`bitbake` are Python; the planner is the fast-moving vendor-heavy half).

## The plan contract must be complete before the SDK stops executing

The plan is the executor's only input; after Phase 4 there is no SDK-side
executor to diff against. All of this lands (additively under `schemaVersion 1`)
while `fan_out` still exists to prove it:

- ✅ **`envAppendPath`** — SDK-owned `EXTRA_ZEPHYR_MODULES` / `PYTHONPATH` append
  (a new key, not stuffed into `env`; "plan wins / CLI fills gaps"; **append only
  if not already present**, matching the reference appenders' de-dup). *Done —
  item 3.*
- **`executionPolicy`** — publish skip-vs-fail (unknown-os = fail, missing-tool =
  skip, `command: null` = skip-with-warning; `orchestrator.py:294-304`).
- **`system-manifest` ordering** — `--emit system-manifest` emits declaration
  order (`manifest.py:37`) while `fan_out` writes sorted; share the ordering, not
  the skip filter (the manifest keeps `os: off` cores).
- **`--core` + `--emit` fail-fast** — `emit_build_plan` has no core-scoping, so
  `--core X --emit build-plan` silently emits every core; reject the combination.
- **slice-independence invariant** — committed contract property *with* its
  precondition: order-free / concurrency-safe **given all `sharedArtefacts` +
  `configArtefacts` are materialised first**; a future firmware-packaging slice
  must preserve it or the invariant version-bumps.
- **required-for-execution key list** — the version-skew guard above.
- **plans are trusted input** — a materialised plan carries absolute host paths +
  `PYTHONPATH`; same trust domain as `board.yaml`; the executor confines writes
  under `buildRoot` and resolves tools by explicit path.
- Fix **#840**; widen `test_emit_build_plan_matches_materialiser` to command +
  env parity.

## Migration — the plan gains everything before anything is deleted

**All four phases are complete.** `tan` (`alplabai/tan-cli`) is the sole
executor and whole command surface; the SDK-side executor and every SDK-side
user command are retired.

1. **Phase 1 (alp-sdk):** complete the contract above; `fan_out` stays and ideally
   consumes the new fields (self-parity). Ship on an SDK tag; delete nothing.
2. **Phase 2 (tan, Hakan):** extract `cli-rs` → the new `tan` repo and **grow it
   to the whole command surface** (build + flash/image/size/renode/clean + the
   manifest I/O those need). The executor conforms to the contract (deletes the
   hand-ported env / skip policy); add **build-validation CI** (SoM matrix,
   plan→execute).
3. **Phase 3 (parity window):** both executors alive; the completeness gate runs
   (see below).
4. **Phase 4 (alp-sdk):** retire `fan_out` / `_dispatch_slice` / the `west alp-*`
   extensions / the `alp` console script; migrate the in-repo consumers (below);
   mark 0014:84-87 superseded; `tan` is the only surface. **No rollback, no
   in-repo oracle after this.**

### Phase-3 parity gate — defined and runnable (satisfied before Phase 4)

"Released `tan` output == `fan_out` output" is undefined and unrunnable as a
slogan (no bitbake-capable CI runners exist; `fan_out` was never a real build
oracle for A-core/yocto — `pr-alp-build.yml` runs the orchestrator with
`continue-on-error`). Define it as:

- (in) **command + env + skip/fail-decision equivalence** between `fan_out` and `tan`'s dry-run of the same plan, over the full SoM matrix, captured toolchain-free (this is exactly the drift that motivated the ADR, and it compares without building);
- (out) **manifest + state byte-parity** on what each writes;
- plus **Zephyr-slice artefact parity** (buildable on today's runners — `pr-renode-aen-smoke.yml` already builds via `west alp-build`);
- yocto/A-core artefact parity is explicitly **out of scope** (no runner infra; bitbake isn't byte-reproducible).

### In-repo consumers Phase 4 must migrate (grep-verified)

- **CI that executes:** `pr-alp-build.yml` (manifest-shape/determinism),
  `pr-renode-aen-smoke.yml` (builds the M55-HP ELF), `pr-renode-dual-os.yml`
  (consumes that ELF) — repoint at a released `tan` binary.
- **Bootstrap / doctor:** `bootstrap.sh` / `bootstrap.ps1` (hard-`die` if
  `west alp-build` missing), `doctor.py` (asserts registration).
- **Manifest/state consumers** (move with the surface to `tan`):
  `flash_backends`, `check_system_manifest.py`, the `flash/image/size/renode/clean`
  commands.
- **Docs (~25):** `README.md`, `AGENTS.md`, `llms.txt`, `getting-started.md`,
  `cli.md`, tutorials, example READMEs — rewrite `west alp-*`/`alp` → `tan`.

### Cross-repo oracle trigger

alp-sdk's planner is the fast-moving half; a planner change that emits fine but
builds wrong must not wait for `tan`'s next unrelated CI. alp-sdk CI fires a
`repository_dispatch` into `tan`'s build-validation workflow (or `tan` crons
against alp-sdk `dev` HEAD) and the result surfaces back as a status on the
alp-sdk PR.

## CI coverage (split)

- **alp-sdk CI:** emit shape / schema / determinism + emit↔`fan_out` parity
  (until Phase 4). No build execution.
- **`tan` CI:** plan→execute build validation over the SoM matrix — build
  correctness lives here; triggered cross-repo on alp-sdk planner changes.
- **alp-sdk-vscode CI:** the extension shells `tan`.

## Consequences

**Good**
- Exactly one executor and one command surface; the #843 drift class is removed
  for HEAD-vs-HEAD, and the skew re-introduction is closed by the
  required-key guard.
- `tan` is a first-class standalone product; installing it never pulls the
  extension (extension → tan → alp-sdk, one-way).
- Rust owns process/cancel/streaming; the SDK sheds all host/process/command
  code; release trains fully decoupled.

**Bad / costs**
- **No in-repo build oracle** — alp-sdk CI asserts plan shape only; build
  correctness is caught in `tan` CI (mitigated by the cross-repo trigger).
- **Reverses 0014:84-87** — pure-`west`-native build without `tan` ends.
- **The plan contract is a fragile single point** post-Phase-4 — mitigated only by
  completing it and proving it in Phase 3.
- **Large, unpriced-until-now scope on Hakan's side:** Phase 2 is not "extract the
  executor" but "extract the **whole command surface** + manifest I/O + stand up a
  new repo's CI/release + build-validation infra (incl. any self-hosted runners
  the SoM matrix needs)."

**Security:** plans are trusted input; the executor confines writes under
`buildRoot`, resolves tools/interpreters by explicit path (never PATH); the
extension verifies the `tan` binary by pinned hash / signature before running it.

## Alternatives

1. **A — executor stays in the SDK (Python).** Keeps standalone-build + the
   in-repo oracle + reversibility. Not chosen (contradicts Hakan's CLI-side ADR;
   "both repos present is fine" removes its edge). **Retained as the fallback** if
   a Phase-1..3 blocker appears before the irreversible Phase 4.
2. **Keep `west alp-build` shelling `tan`** — rejected (a west workspace would
   then depend on another repo's binary, the same 0014:84-87 clause).
3. **Shared Rust crate / Rust planner mirror** — rejected in 0014.

## The one thing that must hold

**Before Phase 4, the proven contract covers BOTH seams with a defined, runnable comparator** — (in) per-slice command + env + skip/fail equivalence over the full
SoM matrix, runnable toolchain-free via dry-run capture; (out) manifest + state
byte-parity — **and an automatic cross-repo trigger runs `tan`'s build validation on every alp-sdk planner change.** `fan_out` is the only thing that can validate
the contract from inside alp-sdk and it is the thing being removed, so
completeness must be proven while it still exists. Green-but-hollow parity
followed by Phase-4 deletion is the failure scenario (fan_out gone, flash/renode/CI
broken, no rollback).

## Open questions / asks for Hakan (resolved — see Status)

1. **`cli-rs` → standalone `tan` repo, grown to the whole command surface** —
   agreed in principle? This is materially larger than "extract the executor";
   it's your code + CI/release + build-validation infra to stand up. Timing is
   yours.
2. **Build-validation CI + the SoM-matrix runner infra** (incl. bitbake for
   A-core) — where does it run? This gates the Phase-3 completeness definition.
3. **Phase-3 parity mechanism** — the toolchain-free command/env/skip comparator +
   manifest byte-parity + the `repository_dispatch` trigger.
4. **`tan` naming** (#837) confirmed as the single surface; the Python `alp`
   console script is retired in Phase 4 so no `alp` remains.
