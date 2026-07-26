# 0021. Toolchain provisioning: pin upstream, never rehost; onboard per lane

Status: Proposed — see **Amendments** below (2026-07-26) for the Arm GNU
Toolchain open-evidence answer and the tokened-toolchain-root-injection
wording correction.
Date: 2026-07-25
Deciders: alpCaner

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

Corrects the Decision section's toolchain-injection mechanism (`:112-116`,
"**Never mutate PATH.** Resolve absolute paths and inject them..."). This is a
correction of *mechanism*, not a reversal of the decision -- the headline and
its consequences still stand; only the word "absolute" was wrong.

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
  toolchain store by SDK version", `:194`) and the Open evidence bullet
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
  ["git", "cmake", "python3"]` — **names only**, no install commands.
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
   above.  Whether it delivers `dtc` / `gperf` on native Windows is unverified
   (see Open evidence).

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
| A — hermetic, no elevation | Zephyr SDK + `arm-zephyr-eabi`, `ninja`, `cmake`, `dtc`, `gperf`, west + pip deps, `tan` | install after one consent click |
| B — needs elevation | J-Link / WinUSB drivers, Linux udev rules, WSL2 enablement | prompt once, **skippable**, and show the command with a "Run for me" button |
| C — licence-gated third party | DEEPX DX-M1 SDK, vendor NPU compilers | never silently fetched; show licence, then verify |

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

- Contents of the Zephyr SDK **Windows** bundle at the pinned version: does
  `west sdk install` deliver `dtc` and `gperf` on win-x64, and does it
  self-extract `.7z` on a host with no 7-Zip?  Test on a clean Windows VM.
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
- [ADR 0020](0020-sdk-owns-build-execution.md) — the SDK plans, tan executes.
- [ADR 0010](0010-heterogeneous-os-orchestration.md) — why Lane 2 is Linux-only.
- `docs/cross-platform-setup.md` — the manual per-OS steps this replaces.
