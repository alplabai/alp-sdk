# 0021. Toolchain provisioning: pin upstream, never rehost; onboard per lane

Status: Proposed — see **Amendments** below (2026-07-26) for the Arm GNU
Toolchain open-evidence answer, the tokened-toolchain-root-injection wording
correction, and the item-2 injection-set / dtc-gperf corrections.
Date: 2026-07-25
Deciders: alpCaner

## Amendment (2026-08-26 — the dtc/gperf-consumer citation named a retired file)

The dtc/gperf open-evidence Amendment below (2026-07-26) names its
consumers as "`scripts/alp_cli/doctor.py`'s `_check_dtc` / `_check_gperf`
hints and `metadata/bootstrap.json`'s `manualInstallHints.windows.note`."
`scripts/alp_cli/doctor.py` is gone — alp-sdk#1367/#1368 deleted it along
with the rest of the `alp_cli` command-line wrappers (ADR 0020's Amendment
item 8). `metadata/bootstrap.json`'s `manualInstallHints.windows.note`
still carries the corrected wording and is unaffected. The host-tool check
itself is now `tan doctor`'s job — a generic host-tools presence check
(`git`/`cmake`/`ninja`/`dtc`/`gperf`/`vendorToolchain`/...,
`python/tan/commands/doctor_cmd.py` in `tan-cli`), not a `dtc`/`gperf`-named
function; this ADR's earlier text should not be read as pinning specific
function names in the live tool.

## Amendment (2026-08-07 — a substitution-set token existing is not the same as it resolving; the Python executor line still hardcodes `toolchain_root=None`)

Corrects the 2026-07-26 "drop `-DCMAKE_MAKE_PROGRAM`..." Amendment's read of
tan-cli#86 (merged the same day) as settling this landing site. It settled
the schema-vs-substitution-set question — `${TOOLCHAIN_ROOT}` is no longer an
*unknown* token — but not the resolution question, and the two are different
blockers.

- **What tan-cli#86 actually ported.** Its files —
  `crates/tan-cli/src/toolchain.rs`,
  `crates/tan-cli/src/commands/build/token_substitution.rs`,
  `crates/tan-cli/src/main.rs`, `crates/tan-cli/src/commands/doctor.rs`,
  `crates/tan-core/src/plan_tokens.rs` — sit entirely under `crates/`,
  tan-cli's FROZEN Rust oracle, kept only as the release-contract reference
  now that the shipping `tan` (from `v0.5.0-rc1`) is the Python program under
  `python/tan/`. The oracle genuinely resolves a toolchain root end-to-end —
  proven by running it (tan-cli#505's evidence block): with
  `ZEPHYR_SDK_INSTALL_DIR` unset it demotes correctly, and on a host where the
  SDK is actually installed it resolves the real path and no demotion
  happens at all.
- **The shipping Python line only got half of it.**
  `python/tan/commands/build/token_substitution.py` recognises
  `${TOOLCHAIN_ROOT}` as a known token name (`:164`), so a plan naming it no
  longer hard-fails with an unknown-token `PlanTokenError` — that half of the
  port landed. The *value* feeding it never did:
  `python/tan/commands/build_cmd.py:1173-1178` (live `dev`,
  `433046c558e0fe0ea55d5f354ee248d413b81be1`, 2026-08-07) reads

  ```python
  # NOT YET PORTED: `crate::toolchain::resolve_toolchain_root`. Left
  # unresolved rather than guessed -- resolution is lazy, so a plan
  # that never names ${TOOLCHAIN_ROOT} (every SDK plan today) is
  # unaffected, and one that does is demoted per its own
  # executionPolicy instead of built against the host root.
  toolchain_root=None,
  ```

  so on the Python executor line every host demotes every slice naming
  `${TOOLCHAIN_ROOT}`, unconditionally — not "when the host has no
  detectable toolchain," which is what the oracle's lazy design and this
  ADR's own wording both imply.
- **The demotion is not the benign skip it sounds like on `--materialise`.**
  On `--execute` it correctly routes through `executionPolicy.missingTool`
  (default skip), the same seam the oracle uses. On `--materialise`,
  `build_cmd.py`'s `_MODE_MATERIALISE` branch binds the `demotions` return
  value and never reads it (tan-cli#505 item 3, measured repro): a demoted
  slice's `configArtefacts` are silently absent from the envelope's
  `written` list, `issues: []`, exit 0, `ok: true` — byte-identical whether
  `executionPolicy.missingTool` is `"skip"` or `"fail"`, i.e. the policy is
  not consulted on this path at all. The repro's own stated consequence: "A
  CI step that materialises then runs `west build` for that core gets
  default Kconfig instead of the project's, i.e. wrong firmware, with no
  signal on either side." That is a data-loss-shaped failure for a
  consumer, not a skipped build.
- **Net effect on this ADR's Lane-1 P1 env-injection half.** Still correctly
  not landed. Not blocked by a closed schema (unchanged: `slices[].env` /
  `slices[].toolchain` stay `additionalProperties: false`,
  `slices[].command.args` stays open) and no longer blocked by an unknown
  token on either the oracle or the Python line's *parser*. It is blocked by
  the Python executor line's *resolver* — `build_cmd.py`'s
  `toolchain_root=None` — which has no tracking issue of its own yet;
  [alp-sdk#1286](https://github.com/alplabai/alp-sdk/issues/1286) records
  this from the alp-sdk side.

## Amendment (2026-08-03 — Python Tan owns the active host checks)

The provisioning policy is unchanged, but exact Tan implementation paths in
the historical body now refer to the frozen v0.4.1 Rust oracle. The active
Python consumers are `python/tan/commands/doctor_cmd.py` (including
`ZEPHYR_SDK_INSTALL_VERSION` and the effective Python-floor checks) and
`python/tan/commands/bootstrap_cmd.py` (including `WORKSPACE_BLOCKING`). Keep
their tan-cli tests in parity with alp-sdk's `metadata/toolchains.json` and
`metadata/bootstrap.json` when either contract changes. The active structured
spelling is `tan doctor --format json`; `tan doctor --json` in the historical
body below names the superseded v0.4 surface.

## Amendment (2026-07-26 — answer the Arm GNU Toolchain open-evidence question)

"Open evidence"'s second bullet below asks whether the Arm GNU Toolchain is
required by any shipping path other than baremetal, framed against the
"Automate the Arm GNU Toolchain install" alternative: if nothing else needs
it, the `manualInstallHints` item *disappears* rather than gets automated.
Answered while fixing `docs/cross-platform-setup.md`'s over-narrow
GD32-only scoping:

- **Three paths need it, so the entry is scoped, not deleted.** Rebuilding
  the E1M-X V2N / V2N-M1 GD32 bridge firmware (`docs/gd32-bridge.md`,
  `docs/bring-up-v2n.md`, `docs/tutorials/07-recovering-a-bricked-bridge.md`);
  building the CC3501E bridge firmware's silicon-free stub target
  (`firmware/cc3501e/README.md`, `firmware/cc3501e/toolchain/arm-none-eabi.cmake`
  -- the CC3501E's *production* image builds with TI `ticlang`, not this
  toolchain); and hand-written bare-metal firmware targeting a real
  M-class core (`ALP_OS=baremetal`, no Zephyr).
- **This ADR's own "other than baremetal" framing rested on an imprecise
  premise.** What CI calls `baremetal` (`.github/workflows/pr-plain-cmake.yml`)
  is a host-toolchain compile smoke -- it configures and builds
  `-DALP_OS=baremetal` with the **host's own** compiler, not a cross
  toolchain, and proves nothing about a cross-compiled bare-metal build.
  No cross-compiled bare-metal recipe ships in-tree today. So "baremetal"
  as written in that bullet does not, by itself, need an Arm toolchain --
  it is hand-written bare-metal firmware *targeting real M-class silicon*
  (which the in-repo job does not exercise) that needs one.
- **Net effect:** the `manualInstallHints` entry stays -- scoped to the
  three paths above, not deleted -- and this remains a documentation-scope
  answer, not a decision to automate the install; that stays future work.
  `docs/cross-platform-setup.md` §2.3/§3.4/§4.3 and
  `metadata/bootstrap.json`'s `manualInstallHints.windows.note` carry the
  three-path scoping; ADR 0012's own Amendment cross-references this one.

## Amendment (2026-07-26 — tokened toolchain-root injection, not absolute paths)

Corrects the Decision section's toolchain-injection mechanism (the
**"Never mutate PATH."** paragraph: "Resolve absolute paths and inject
them..."). This is a correction of *mechanism*, not a reversal of the
decision -- the headline and its consequences still stand; only the word
"absolute" was wrong.

- **Read "resolve absolute paths and inject them" as tokened
  toolchain-root injection, resolved by the executor.** The build-plan
  already has the right mechanism for this, just not applied here yet:
  `metadata/schemas/build-plan-v1.schema.json`'s `planPathMode` (issue #865)
  states that the `tokened` mode means "every checkout/project-anchored absolute
  path this plan would otherwise embed is instead a literal `${SDK_ROOT}` /
  `${PROJECT_ROOT}` / `${PYTHON}` token ... tan-cli (PR #24) substitutes them
  at materialise time, rather than the SDK baking in this run's absolute
  checkout path." A toolchain root is the same class of fact: a
  `${TOOLCHAIN_ROOT}`-style token emitted in the plan, substituted by `tan`
  at materialise time exactly like the three tokens above -- not a literal
  absolute path baked in by alp-sdk.
- **"Never mutate PATH" and both stated consequences still stand.** No
  editor restart after an install, and two SDK versions coexisting on one
  host, are both delivered by tokened injection exactly as they would be by
  literal-absolute-path injection -- the token still resolves to an
  absolute path on the executor's host at the moment of use, and PATH is
  still never touched. Only the wire representation of that path was wrong.
- **This has no alp-sdk-only slice.** Both landing sites are closed,
  `additionalProperties: false` contracts: `slices[].toolchain`
  (`build-plan-v1.schema.json`) requires exactly
  `[targetTriple, compiler, sysroot, id]`, and `slices[].env` permits only
  `ALP_SDK_ROOT`. Widening
  either is a Wave-C contract edit, and `tan-core`'s build-plan parser
  (`crates/tan-core/src/build_plan.rs`) models only the core slice today --
  zero references to `toolchain` anywhere in it -- so an alp-sdk-side emit
  would be a schema field nothing on the consumer side reads. It needs a
  coordinated `tan` change first, and per [ADR 0020](0020-sdk-owns-build-execution.md)'s
  Amendment item 3, a Wave-C contract edit at unchanged `schemaVersion` must
  ship additive-optional with a strict-producer / tolerant-consumer rollout,
  the same way `executionPolicy` was corrected there.
- **The two Lane 1 P1 halves separated in practice.** This ADR's Decision
  fuses "the `sha256` lockfile" and "build-plan path injection" into one
  Lane 1 P1 bullet. Only the lockfile half shipped
  (`metadata/toolchains.json`, #962) -- it had a self-contained alp-sdk
  deliverable; the injection half does not, for the reason above, and stays
  open.
- **Footprint figures corrected.** The Alternatives entry ("Key the
  toolchain store by SDK version") and the Open evidence bullet
  below both carry an unmeasured estimate -- ~1 GB (minimal SDK +
  `arm-zephyr-eabi`) to ~17 GB (full bundle) -- sourced from a
  `pr-twister.yml` comment, never a measurement. Measured, linux-x86_64,
  minimal SDK + `arm-zephyr-eabi` (`metadata/toolchains.json`'s
  `measuredFootprint`): **172403720 bytes** compressed download (~164 MiB)
  and **2026739200 bytes** extracted on disk (~1.89 GiB), of which
  `hosttools/` is **1242632545 bytes** and `gnu/arm-zephyr-eabi/` is
  **784086497 bytes**. The estimate was wrong in both directions at once
  because it conflated the two numbers: ~6x too high on download, ~2x too
  low on disk.
- **Design consequence the measurement exposes.** `hosttools/` is 61% of
  the on-disk footprint -- larger than the compiler itself -- and it is what
  carries `dtc`. `west sdk install` installs it by default
  (`scripts/west_commands/sdk.py:452`:
  `if not args.interactive and not args.no_hosttools`). `--no-hosttools`
  nearly halves the on-disk store but loses `dtc`, which is exactly this
  ADR's still-open `dtc`/`gperf`-on-Windows question below. Recorded as a
  trade-off; not resolved here.

## Amendment (2026-07-26 — drop `-DCMAKE_MAKE_PROGRAM` from item 2's injection set; correct the previous amendment's schema claim; answer the dtc/gperf open-evidence bullet)

Three corrections; none reverses the Decision's headline.

- **Item 2's injection triple becomes a pair.** The Decision's **"Never
  mutate PATH."** paragraph names `-DCMAKE_MAKE_PROGRAM`,
  `ZEPHYR_SDK_INSTALL_DIR`, and `ZEPHYR_TOOLCHAIN_VARIANT`. Drop
  `-DCMAKE_MAKE_PROGRAM`: there is nothing for it to point at. `ninja` is a
  `prerequisites.windows` / winget tool (`metadata/bootstrap.json`), not a
  Zephyr SDK artifact -- this ADR's Context section already says so
  ("`west sdk install` does not ship `ninja`"), and
  `metadata/toolchains.json` lists only the minimal SDK and
  `arm-zephyr-eabi`. So a `${TOOLCHAIN_ROOT}/.../ninja`-style token would
  resolve to nothing on every host, and tan's contract deliberately folds a
  blank toolchain root to unresolved precisely so such a path cannot degrade
  silently. The real problem `-DCMAKE_MAKE_PROGRAM` appeared to solve -- an
  editor spawned before winget's `PATH` update lands, so CMake's generator
  search misses `ninja` -- is a host-runtime fact only the executor knows at
  spawn time; under ADR 0020's split that is tan's business, not plan
  content. The remaining pair, `ZEPHYR_SDK_INSTALL_DIR` /
  `ZEPHYR_TOOLCHAIN_VARIANT`, is unaffected -- both point at a real,
  `metadata/toolchains.json`-pinned Zephyr SDK artifact.
- **The Tier A row and the Decision's package-manager paragraph were never
  in tension -- Tier A's label is about elevation, not delivery
  mechanism.** The Tier A table's Contents column lists `ninja`, `cmake`,
  `dtc`, and `gperf` alongside the Zephyr SDK and `arm-zephyr-eabi`; the
  Decision's **"Signed package manager stays primary for the small host
  tools"** paragraph names `ninja` as one of those tools, and the dtc/gperf
  bullet below establishes that `dtc` and `gperf` are winget-delivered too
  -- so, read as "hermetic archive contents," the row disagrees with the
  Decision text on three tools, not the one this ADR flagged before. But
  Tier A's Rule column is "install after one consent click" -- a statement
  about *elevation and consent friction*, not about *which mechanism*
  delivers the tool. A user-scope winget install satisfies "no elevation,
  one consent click" exactly as a hermetic-archive unpack would, so the row
  and the Decision paragraph were never actually opposed; the row was
  underspecified about mechanism, not wrong about tier membership. Marked
  at the table itself, not just asserted here: the Tier A row now carries a
  footnote naming `ninja`, `dtc`, and `gperf` as package-manager-delivered
  and forward-pointing to this amendment, so a reader going top-down from
  the table hits the pointer instead of an unmarked ambiguity.
- **Correcting the previous ("tokened toolchain-root injection") Amendment's
  schema claim.** It states "both landing sites are closed,
  `additionalProperties: false` contracts", naming `slices[].toolchain` and
  `slices[].env`. That is accurate for those two, but with
  `-DCMAKE_MAKE_PROGRAM` dropped, the landing site that actually mattered
  for it was never either of those -- it was `slices[].command.args`
  (`build-plan-v1.schema.json`), a plain
  `{"type": "array", "items": {"type": "string"}}`: an open, unconstrained
  string array, not a closed contract needing a Wave-C schema edit to
  accept a new argument value.
  The previous Amendment's *conclusion* -- that this needed a coordinated
  `tan` change first -- still held, but for a different reason than the one
  it gave: the `${TOOLCHAIN_ROOT}` token simply did not exist in tan's
  substitution set until tan-cli#86, not because a schema was closed
  against it. The "closed contract" reasoning should not be inherited into
  future readings of that Amendment for this landing site.

  Schema-open is not gate-free, though. Seam-1 verifies command and env
  **shape** against the frozen oracle (ADR 0020's Amendment item 4 retune),
  so any new arg landing in `slices[].command.args` (or a new key landing in
  `slices[].env`) needs a comparator normalization entry in
  `tests/parity/seam1_field_diff.py` (confirmed present at that path) *and*
  its tan-cli vendored twin (`tan-cli/tests/parity/seam1_field_diff.py`,
  kept in lockstep per that Amendment's own instruction), plus regenerating
  `tests/fixtures/emit-snapshots/*.build-plan.snap`
  (`scripts/check_emit_snapshots.py`) -- otherwise a live plan carrying the
  new arg either silently diverges from the frozen oracle seam-1 diffs
  against, or the emit-snapshot gate flags stale goldens. (`*.zephyr-conf.snap`
  holds generated Kconfig content, unaffected by a command-arg change, so it
  is not part of this regeneration.)
- **Answering the Open evidence dtc/gperf bullet.** Settled without a clean
  Windows VM. `hosttools_windows-x86_64.7z` is the right asset to inspect
  because `west sdk install` installs hosttools by default unless
  `--no-hosttools` (`scripts/west_commands/sdk.py:452`, already established
  in the 2026-07-26 "tokened toolchain-root injection" Amendment above),
  not because it happens to be the only asset published. It is a
  **separately published** sdk-ng v1.0.1 asset (104312951 bytes),
  `sha256`-verified against upstream's own published `sha256.sum`
  (`b5aca806f7d3de696317db1c3902003421d366a083cb8d68f276c60f655ceaf0`), and
  listed with `7z l`: **1486 entries, 21 executables** (`openocd.exe`, 19x
  `qemu-*.exe`, `wget.exe`) -- **zero** matches for `dtc`, `gperf`,
  `device-tree`, or `devicetree` anywhere in the listing. The equivalent
  Linux hosttools bundle at the same SDK version
  (`hosttools/sysroots/x86_64-pokysdk-linux/usr/bin/dtc` in a local
  extracted `zephyr-sdk-1.0.1` tree) **does** ship `dtc`. So, for the two
  hosts actually inspected (`windows-x86_64`, `linux-x86_64`): the Zephyr
  SDK ships `dtc` on Linux but not on Windows, and ships `gperf` on
  neither -- `macos-aarch64`, also pinned in `metadata/toolchains.json`,
  was not inspected here. This is consistent with Zephyr's own
  native-Windows guide winget-installing `oss-winget.dtc` and
  `oss-winget.gperf` as a load-bearing step, not redundancy. Method
  recorded because it is reusable and cheaper than this ADR assumed:
  `hosttools_*` is a separately published asset, so the question was
  answerable by downloading and listing it from any host with `7z` -- no
  clean Windows VM required. Consumers:
  `scripts/alp_cli/doctor.py`'s `_check_dtc` / `_check_gperf` hints and
  `metadata/bootstrap.json`'s `manualInstallHints.windows.note` now state
  this plainly instead of the (false) "bundled with the Zephyr SDK on
  Windows" claim they carried before. The `.7z`-self-extraction half of
  that same Open evidence bullet stays answered-as-already-recorded, not
  newly answered here: `manualInstallHints.windows.note`'s pre-existing
  "7-Zip must already be on PATH..." line already states west delegates
  `.7z` extraction to patoolib, which shells out to an external
  7z/7za/7zr/7zz/7zzs/unar binary with no pure-Python fallback. The Arm GNU
  Toolchain bullet is untouched, exactly as the first (2026-07-26) Amendment
  above left it.

  This does not resolve the toolchain-store keying question (Alternatives,
  "Key the toolchain store by SDK version") or the Lane 0 prebuilt-blink
  question -- only the dtc/gperf bundle-contents question is settled.

## Context

Alp Lab sells modules, and a module vendor is judged on **time-to-first-blink**:
how long a customer takes from an unboxed EVK to running code, with no
documentation read.  NXP (MCUXpresso Installer) and Nordic (nRF Connect
Toolchain Manager) both ship one-click host provisioning.  A vendor whose
tooling is worse loses the design-in even when the silicon is better, and every
field-application hour spent on "install ninja" is margin gone.

Today the SDK stops one layer short of that.  `tan bootstrap` (native,
cross-platform since tan-cli#55) owns the Python layer — `.venv`, `west`,
`west init -l` + `west update`, `west zephyr-export`, Zephyr's
`requirements.txt`, the SDK's pip extras — and needs no elevation.  Everything
*below* that layer is declared but never installed:

- `metadata/bootstrap.json` declares `prerequisites.windows:
  ["git", "cmake", "python", "ninja"]` and `prerequisites.posix:
  ["git", "cmake", "python3", "ninja", "xz", "wget"]` — **names only**, no
  install commands.
- tan hardcodes the winget IDs per tool
  (`crates/tan-cli/src/commands/bootstrap/steps.rs`: `Git.Git`,
  `Kitware.CMake`, `Python.Python.3.12`, `Ninja-build.Ninja`), formats them
  into `"Missing required tools:"`, and exits non-zero.  It never runs them.
- `manualInstallHints.windows` states the policy outright: the Arm GNU
  Toolchain and the Zephyr SDK (`west sdk install`) are *"separate manual,
  one-time installs on native Windows -- not auto-installed by bootstrap.ps1"*.
- `docs/cross-platform-setup.md` §4.1 (winget lines) and §4.3 (download the Arm
  installer from arm.com by hand) are the human workaround for exactly this.

Observed on a real Windows 11 host, 2026-07-25: SDK v0.13.0 installed and
active, `cmake` / `git` / `python 3.11` / `winget` present, `ninja` absent —
bootstrap refused.  `dtc` and `gperf` were also absent.  The VS Code task
terminal rendered tan's non-zero exit as `failed to launch (exit code: 1)`,
which reads as if the binary never started.

Two facts constrain the fix more than they first appear:

1. **The extension already has fix plumbing.**  `src/toolchain.ts`'s
   `runToolchainFix(fixId)` calls `fixCommand(fixId, host())` from
   `@alp-sdk/core/toolchain/bootstrapPlan` and handles three kinds —
   `pointer` (open a URL), `guide` (per-OS quick-pick; the current host's
   command is runnable in a terminal), `bootstrap` — and
   `src/toolchain/doctorPanel.ts` already routes a `runToolchainFix` message.
   The missing-tool path simply never reaches it.
2. **`west sdk install` does not ship `ninja`.**  It is the right tool for the
   Zephyr SDK and `arm-zephyr-eabi`, but it would not have fixed the failure
   above.  It does not deliver `dtc` or `gperf` on native Windows either
   (verified; see Open evidence and the 2026-07-26 "drop
   `-DCMAKE_MAKE_PROGRAM`..." Amendment above).

## Decision

**Ownership.** Provisioning *logic* lives in tan; the *pins and install
commands* live in alp-sdk metadata; the extension is a renderer over
`tan doctor --json`.  This is ADR 0020's split applied one layer down — a
hardcoded install table in tan is the same hand-port drift RFC #843 already
cost us once.

**Do not rehost toolchain binaries.**  Pin upstream canonical URLs plus a
`sha256` per artifact in a lockfile and download from origin.  Mirror only as
an explicit enterprise/offline fallback, with corresponding source alongside.

**Key the cache by artifact, not by SDK version.**
`~/.alp/toolchains/<artifact>-<version>/` (e.g. `zephyr-sdk-0.17.0-arm-zephyr-eabi`),
and each SDK version's lockfile *references* artifacts.  `ALP_TOOLCHAIN_ROOT`
overrides the root for bench machines and CI.

**Signed package manager stays primary for the small host tools** (`git`,
`cmake`, `ninja`) — hermetic archives are primary only for the large pinned
compilers, which `west sdk install --install-dir` already delivers
hermetically.  Vendoring the Python interpreter is worth it regardless: it
kills the Microsoft-Store alias and the version-drift traps.

**Never mutate PATH.**  Resolve absolute paths and inject them
(`-DCMAKE_MAKE_PROGRAM`, `ZEPHYR_SDK_INSTALL_DIR`,
`ZEPHYR_TOOLCHAIN_VARIANT`).  The injection is *emitted by alp-sdk's
build-plan* and *consumed by tan*.  Consequences: no editor restart after an
install, and two SDK versions coexist on one host.

**Three tiers, one consent screen** listing artifact, source, size and licence:

| Tier | Contents | Rule |
|---|---|---|
| A — hermetic, no elevation | Zephyr SDK + `arm-zephyr-eabi`, `ninja`\*, `cmake`, `dtc`\*, `gperf`\*, west + pip deps, `tan` | install after one consent click |
| B — needs elevation | J-Link / WinUSB drivers, Linux udev rules, WSL2 enablement | prompt once, **skippable**, and show the command with a "Run for me" button |
| C — licence-gated third party | DEEPX DX-M1 SDK, vendor NPU compilers | never silently fetched; show licence, then verify |

\* `ninja`, `dtc`, and `gperf` are delivered by the signed package manager
(winget/apt/brew), not unpacked from a hermetic archive -- see the
2026-07-26 "drop `-DCMAKE_MAKE_PROGRAM`..." Amendment above. Tier A
membership here means "no elevation, one consent click," not "shipped as
a hermetic archive" -- delivery mechanism and elevation are independent
axes, and this row was never actually in tension with the Decision's
package-manager paragraph below.

The rule is **never *require* copying a command** — not "never print one".
Corporate security teams want to see what will run, and J-Link's licence likely
forbids silent redistribution without a Segger agreement.

**`tan doctor --json` is a versioned contract.**  It rides the existing
envelope (`{command, ok, exitCode, project, data, issues}`) and gets a golden
fixture + key-set conformance gate, the same protection the `--emit kconfig`
contract has.  The Toolchain Doctor panel becomes the onboarding surface: every
requirement with a state (Ready / Will install / Needs you), one "Fix all", a
per-row "why".

**Sequence per lane, not globally.**  The three customer paths have different
first rungs, and a single P0/P1/P2 list mis-prioritises two of them.

- **Lane 0 — prebuilt blink (runs parallel to everything).**  A signed
  prebuilt image per SKU, flashed by `tan` with an extension button, before any
  toolchain exists.  Proves board, cable, driver and flash path in minutes and
  decouples first success from a 1 GB+ download.  This is the highest-leverage
  item and it is absent from the SDK today.
- **Lane 1 — Zephyr on Cortex-M.**
  - *P0a (extension)* — run `tan doctor` **before** spawning the bootstrap
    terminal and render the envelope's `issues` as Fix buttons through the
    existing `runToolchainFix`.  A missing `ninja` stops reading as
    `failed to launch (exit code: 1)`.  Small delta; the plumbing exists.
  - *P0b (alp-sdk + tan)* — `prerequisites.install.{windows,posix,macos}` in
    `metadata/bootstrap.json` (the shape `nativeLibHints.command` already
    uses), and `tan doctor --fix` executes them.  This is what actually fixes
    the observed host.
  - *P1* — `west sdk install --install-dir` into the artifact-keyed store, the
    `sha256` lockfile, and build-plan path injection.
- **Lane 2 — Yocto on Cortex-A** (RZ/V2N, i.MX 93).  First rung is a
  **published container image** (plus WSL2 distro provisioning on Windows), not
  archives: a home-directory archive set cannot express a Yocto host.  Per
  ADR 0010 and ADR 0012 this lane is Linux-only by upstream constraint.

**Cross-cutting requirements** for the downloader and doctor, cheap now and
expensive as support tickets later:

- Honour the system proxy and custom CA bundles.  A TLS-intercepting middlebox
  rewrites a download and the `sha256` fails — the error must say
  *proxy/CA interference*, never *corrupt download*.
- Resumable partial downloads and idempotent re-entry; a consent flow on a slow
  link will be abandoned mid-way.
- Doctor checks `LongPathsEnabled`, spaces in `%USERPROFILE%` (Zephyr has
  historically broken there), non-admin accounts (Tier A must need zero
  elevation — winget user-scope installs comply), and `windows-arm64`, which has
  **no official Zephyr SDK host build** and must be routed to WSL2-aarch64 with
  a clear message.  Apple silicon is fine.
- Two users on one bench machine duplicate the per-user store.  Acceptable;
  `ALP_TOOLCHAIN_ROOT` is the escape hatch.

## Alternatives

**Rehost every artifact on Alp Lab infrastructure.**  Buys immunity to upstream
URL churn and one origin for air-gapped export.  Rejected: GPLv3 corresponding-
source obligations for the GCC-based toolchains, an Arm EULA review, bandwidth
and uptime we would own, a download domain with zero AV reputation, and TLS-
intercepting middleboxes distrusting it.  Pinning upstream inherits upstream's
provenance and carries no legal surface; its only real loss — an upstream
deleting a release — is covered by the fallback mirror.

**Hermetic archives for everything, including the small tools.**  Best on
reproducibility.  Rejected for the small tools: Windows Defender and SmartScreen
quarantine unsigned `ninja.exe` / `arm-*-gcc.exe` unpacked into a user profile,
a failure discovered only on customer hosts.  Kept for the large pinned
compilers, which are `sha256`-verified and SDK-managed.

**Key the toolchain store by SDK version** (`~/.alp/toolchains/<sdk-version>/`).
Rejected: duplicates roughly 1 GB (minimal SDK + `arm-zephyr-eabi`) to ~17 GB
(full bundle) per SDK release, and migrating customer machines off a wrong
layout later is miserable while choosing the right one now is free.  Keyed by
artifact, the matrix scales with *architectures and NPU vendors*, not SKUs —
one Cortex-M toolchain serves every Alif/Renesas/NXP module.

**One global P0/P1/P2 ordering.**  Rejected: it front-loads the Zephyr lane and
files containers as polish, but for an A-core customer the container *is*
onboarding.

**`west sdk install` as the first step.**  Rejected as *first*: it does not
ship `ninja`, so it does not fix the failure we actually observed.  Retained as
Lane 1 P1 — Apache-2.0, upstream URLs, `--install-dir` and toolchain subsetting
already proven by our own `pr-twister.yml`
(`west sdk install --gnu-toolchains arm-zephyr-eabi --no-hosttools`), and it may
remove the separately-managed Arm GNU Toolchain from the Zephyr path entirely.

**Automate the Arm GNU Toolchain install.**  Deferred pending evidence: if no
shipping path other than baremetal needs it, the `manualInstallHints` item
*disappears* rather than gets automated — the cheapest fix available.

## Open evidence

These block P1 tickets, not P0:

- ~~Contents of the Zephyr SDK **Windows** bundle at the pinned version:
  does `west sdk install` deliver `dtc` and `gperf` on win-x64, and does it
  self-extract `.7z` on a host with no 7-Zip?  Test on a clean Windows
  VM.~~ **Answered** — see the 2026-07-26 "drop `-DCMAKE_MAKE_PROGRAM`..."
  Amendment above: for the hosts inspected (`windows-x86_64`,
  `linux-x86_64`), neither `dtc` nor `gperf` ships in the Windows hosttools
  bundle — the archive `west sdk install` installs by default — verified
  via `7z l` against a `sha256`-checked `hosttools_windows-x86_64.7z`, no
  clean Windows VM needed. The `.7z`-self-extraction half was already
  answered before this amendment, not newly here:
  `manualInstallHints.windows.note`'s pre-existing "7-Zip must already be
  on PATH..." line already states west delegates `.7z` extraction to
  patoolib, which shells out to an external 7z/7za/7zr/7zz/7zzs/unar binary
  with no pure-Python fallback.
- Is the Arm GNU Toolchain required by any shipping path other than baremetal?
- ~~Real per-artifact sizes for our minimal set.  The ~17 GB figure is a
  comment in `pr-twister.yml`, not a measurement.~~ **Answered** — see the
  2026-07-26 "tokened toolchain-root injection" Amendment above;
  `metadata/toolchains.json`'s `measuredFootprint` carries the real numbers.

## Consequences

Good:

- First success stops depending on a toolchain download at all (Lane 0), and
  the common Windows blocker becomes a button instead of a printed command.
- No PATH mutation means no editor restart and no "reopen PowerShell" step, and
  side-by-side SDK versions become possible rather than accidental.
- Install commands live in one place all three executors read, so
  `bootstrap.sh`, `bootstrap.ps1` and tan cannot drift.
- The `sha256` lockfile, embedded in `tan support-bundle`, lets a customer prove
  their CI matches ours — what an industrial qualification audit asks for.
- Offline export/import is nearly free once Tier A is archives.

Bad / costs:

- A lockfile per SDK release is a new maintenance surface, and upstream URL
  churn now breaks builds until the pin moves.
- The consent screen and Tier B elevation flow are real UI work that cannot be
  skipped without breaking corporate-host trust.
- Tier C stays manual by law, so NPU onboarding remains multi-step.
- Doctor grows host-specific checks (long paths, spaced home, arm64) that only
  earn their keep on customer machines we cannot test directly.

## Related

- [ADR 0012](0012-cross-platform-developer-host.md) — cross-platform developer
  host; Linux required only for Yocto.  This ADR automates what 0012 documented.
- [ADR 0020](0020-sdk-owns-build-execution.md) — Python Tan owns the normal
  relocated planner and executor; alp-sdk retains the reference producer.
- [ADR 0010](0010-heterogeneous-os-orchestration.md) — why Lane 2 is Linux-only.
- `docs/cross-platform-setup.md` — the manual per-OS steps this replaces.
