# Python `tan` — core executor MVP (sub-project 1 of 4)

Date: 2026-07-29
Status: Draft — for review
Deciders: alpCaner (alp-sdk), Hakan (tan-cli, alp-sdk-vscode)
Relates to: [ADR-0020](../../adr/0020-sdk-owns-build-execution.md) (this work
supersedes its language split; a superseding ADR-0022 is written once this MVP
retires the open risks)

## Why

Today the toolchain spans two languages either side of a versioned JSON seam:

```
alp-sdk (Python planner) ──build-plan JSON──► tan (Rust executor) ──envelope──► alp-sdk-vscode
```

That seam is not free. It costs a hand-synced parity comparator
(`tests/parity/seam1_field_diff.py` **and** its vendored twin inside tan-cli,
kept in lockstep by hand), a version-skew required-key guard, a cross-repo
`repository_dispatch` oracle that is still not standing (#855), and an
additive-only `schemaVersion 1` evolution discipline on every planner change.
ADR-0020 records this itself under *Bad / costs*: the plan contract is
"a fragile single point".

**The end-state:** planner *and* executor live in one Python program, `tan`.
alp-sdk keeps C/HAL, metadata, examples and firmware — and no Python. The
build-plan stops being a versioned inter-process contract and becomes an
in-process data structure, so the comparator, the skew guard and the #855
oracle are deleted rather than maintained.

The vscode↔`tan` envelope survives — it is a *process* boundary, not a language
boundary — but it is one contract instead of two.

### Why this is now viable

`tan` is the **sole hard consumer** of the build-plan. Studio's build-worker
runs `west build` directly and treats `alp build --plan` as purely additive
telemetry ("if the planner fails, the build proceeds and `planJson` stays
null"), and it already shells the Python `alp validate` surface. Nothing else
hard-depends on the plan JSON, so collapsing the seam strands no consumer.

## Scope of this sub-project

This spec covers **sub-project 1 only** — the risk-retiring MVP. The full port
is decomposed as:

| # | Sub-project | Delivers |
|---|---|---|
| **1** | **Core executor MVP** (this spec) | `build` / `build --native` in Python, hash-pinnable packaging, the parity oracle — on AEN + native_sim |
| 2 | Planner move | `alp_orchestrate` + validators/generators relocate into `tan`; the JSON seam collapses; comparator + skew guard + #855 machinery deleted |
| 3 | Surface breadth | flash / image / size / renode / clean + manifest I/O + the ~13 non-SDK commands |
| 4 | Cutover | vscode bundling, Studio, alp-sdk CI/docs/bootstrap repoint; Rust `tan` retired |

Sub-project 1 exists to answer three questions cheaply, **before** the planner
move touches alp-sdk or the cutover touches Hakan's extension:

1. Can a Python `tan` ship as a **hash-pinnable, signable single artifact**?
2. Does it reproduce the vscode **envelope** byte-for-byte?
3. Can we port **against a live oracle**, so no drift window opens?

If any answer is no, we learn it here and the remaining three sub-projects are
never started.

## Non-goals (explicitly out of scope for sub-project 1)

- Moving the planner. alp-sdk keeps its Python planner untouched; Python `tan`
  invokes `--emit build-plan` exactly as Rust `tan` does today. The seam
  collapse is sub-project 2.
- Any command beyond `build` and `build --native`.
- Any change to alp-sdk-vscode, Studio, or alp-sdk CI.
- Retiring Rust `tan`. It stays shipped and authoritative throughout — it is
  the oracle.
- V2N / NX9 / Yocto slices. AEN + native_sim only.

## Architecture

A new Python package inside the tan-cli repo, alongside the Rust workspace,
mirroring the Rust module split so parity review is a file-to-file read:

```
tan-cli/
  crates/            # unchanged, still shipped, still the oracle
  python/
    tan/
      __main__.py       # Typer app, `tan` entrypoint
      cli.py            # command surface (mirrors crates/tan-cli/src/cli.rs)
      envelope.py       # {command,ok,exitCode,project,data,issues} + exit codes
      core/             # pure domain — mirrors crates/tan-core
        build_plan.py   # plan consumer model + version-skew guard
        plan_exec.py    # apply_env_append / assemble_slice_env / resolve_action
      commands/
        build/          # native.py preflight.py execute.py materialise.py …
    tests/
      parity/           # the oracle harness (see below)
```

**`core/` is pure and IO-free**, matching the repo's existing
`keep-files-small; pure logic in tan-core, not the IO/executor file` rule. The
env-append de-dup and the skip-vs-fail policy are pure functions with unit
tests, exactly as `tan-core::plan_exec` has them.

### Framework choice

- **Surface: Typer.** Maps cleanly onto the existing `Commands` enum +
  per-command `XxxArgs` shape, and gives `--help` / completion for free.
- **Executor internals: stdlib `subprocess`.** No new dependency for process
  control. West itself is Python driving hour-long cmake/ninja/bitbake runs, so
  spawn / stream / cancel is a solved problem in this ecosystem, not research.
- **Progress:** a single small library (`rich`), replacing `indicatif`.

ADR-0020 claims "Rust owns process/cancel/streaming natively" as a benefit. We
treat that as a **capability** question that west already answers, and address
the *identity* half — one hashable artifact — under Packaging below.

## The three MVP deliverables

### 1. Executor: `build` and `build --native`

The full pipeline for a plan, contract-driven — no hand-ported behaviour:

- **Consume** the plan (`schemaVersion 1`). Top-level keys `schemaVersion`,
  `generatedBy`, `boardYaml`, `sku`, `buildRoot`, `slices`, `sharedArtefacts`,
  `warnings` are required; `executionPolicy`, `planPathMode`, `sdkVersion`,
  `sdkCommit` are optional-but-always-emitted (strict producer / tolerant
  consumer).
- **Version-skew guard**: reject an unsupported `schemaVersion`, and fail loudly
  on an unknown required key rather than falling back to hand-ported behaviour.
  That fallback is precisely the RFC #843 drift.
- **Token substitution**: `planPathMode: "tokened"` is required; substitute
  `${SDK_ROOT}` / `${PROJECT_ROOT}` / `${PYTHON}`.
- **Materialise** `sharedArtefacts` then each slice's `configArtefacts` **before**
  any slice runs — that ordering is the precondition of the plan's
  slice-independence invariant.
- **Env**: `env` set verbatim; `envAppendPath` appended `os.pathsep`-wise
  **only if not already present** (de-dup). "Plan wins / CLI fills gaps."
- **Policy**: honour `executionPolicy` — `unknownBackend`=fail,
  `missingTool`=skip, `nullCommand`=skip — falling back to built-in behaviour
  when an older plan omits it.
- **Consumer-side resolution stays consumer-side**: `ZEPHYR_BASE` and the west
  venv are deliberately *not* in the plan. Port the existing resolution (walk
  `.venv` up the tree, `$ZEPHYR_BASE/../.venv`, `<sdk-parent>/.venv`,
  `<sdk-parent>/zephyrproject/.venv`).
- **Execute** each slice, streaming output, honouring cancellation.

Per-slice fields consumed: `coreId`, `backend`, `buildDir`, `appDir`,
`configArtefacts`, `toolchain`, `artifacts`, `debug`, `command`, `env`,
`envAppendPath`.

### 2. Packaging — one raw executable per target

**Correction to ADR-0020 (verified against the extension source).** ADR-0020
states the extension "Bundles or downloads the `tan` binary with **pinned-hash /
signature verification**", and its security clause calls that download "the
softest link… it executes arbitrary build commands". **That verification does
not exist.** `alp-sdk-vscode/src/alpCli/download.ts:11-16` disclaims it in its
own comment — verbatim: *"there's no checksum/signature check here"* — pointing
at an open tracking issue, `alplabai/tan-cli#7`. The only integrity check is a
byte-count match against `Content-Length` (`download.ts:100-123`); the bundled
path is trusted purely because it shipped inside the `.vsix`
(`adapterCore.ts:106-110` chmods it and runs it). The PATH path's "verified"
means only a `--version` probe matching a regex (see below).

So there is **no hash-pin to preserve** — the ADR describes an intent, not the
shipped state. Packaging is therefore not a security-regression risk. It is a
**shape** requirement, which is a harder constraint than it sounds:

`service.ts:293-314` builds the download URL as
`https://github.com/alplabai/tan-cli/releases/download/v${version}/tan-${target}${".exe" on win32}`,
and its comment states verbatim: *"tan-cli ships a RAW binary per target (not an
archive)"*. `download.ts:159-162` writes the response **straight to the
destination file — there is no unarchive/unpack step anywhere in the
extension.**

**Requirement: PyInstaller `--onefile`.**

- A **one-file** artifact drops in unchanged — same URL scheme, same
  download→rename→`chmod 0o755` (`download.ts:124-129`)→cache-at-one-path flow.
- A **one-dir** artifact **breaks the extension.** There is no code path that
  can materialise a directory of files. One-dir is out of scope, permanently,
  unless the extension's download path is rewritten.

**Release assets must keep the existing names and targets** (`service.ts:34-46`)
— six targets, named by Rust target triple even though the producer is no longer
Rust:

| platform/arch | asset name |
|---|---|
| `win32/x64` | `tan-x86_64-pc-windows-msvc.exe` |
| `win32/arm64` | `tan-aarch64-pc-windows-msvc.exe` |
| `linux/x64` | `tan-x86_64-unknown-linux-musl` |
| `linux/arm64` | `tan-aarch64-unknown-linux-musl` |
| `darwin/x64` | `tan-x86_64-apple-darwin` |
| `darwin/arm64` | `tan-aarch64-apple-darwin` |

Keeping the triple-shaped names means **zero extension change** for the
MVP. (The `musl` names imply a static-linking property PyInstaller does not
provide; renaming them is a follow-up that touches `service.ts:34-46`, so it is
deliberately deferred out of sub-project 1.)

This does **not** claim `tan` becomes Python-free at runtime for *building*: a
build always needs the host's west/Zephyr Python environment. That is equally
true of Rust `tan` today (ADR-0020 refuses a Rust planner precisely because
west/bitbake are Python). The bundled interpreter runs `tan` itself; the west
venv runs the build. Keeping those two interpreters distinct and explicit is a
requirement, not an accident — resolve every tool by explicit path, never PATH.

**Acceptance:** a signed single-file `tan` on Linux/macOS/Windows whose SHA-256
the extension can pin; startup overhead measured and recorded.

### 3. The parity oracle — port against a live reference

Rust `tan` is shipped, working, and stays installed. For every ported
capability we diff **Python `tan` vs Rust `tan`** on the same plan:

- **envelope parity** — `{command, ok, exitCode, project, data, issues}` compared
  as parsed JSON (key order irrelevant, values exact), plus the process exit
  code;
- **execution parity** — per-slice resolved `command`, resolved `env`, and the
  skip-vs-fail decision, captured **toolchain-free via dry-run**. This is the
  same shape as the seam-1 comparator (`command`, `env`, `appDir`,
  skip/fail-decision, `debug.probe`), reused rather than reinvented;
- **artefact parity** — for the real AEN build, the materialised artefact set
  and the produced `zephyr.elf` / `zephyr.bin` exist and the build succeeds.

Rust `tan` is retired only in sub-project 4, and only for capabilities the
oracle has confirmed. **No oracle-less window opens at any point** — this is the
direct mitigation of the "reverse drift, no reference" risk, and it is why the
port is safe in a way the original Phase 4 deletion was not.

## Constraints the extension imposes (verified in `alp-sdk-vscode`)

These are contract facts the Python `tan` must satisfy exactly, or the extension
fails **silently** — it renders nothing, with no compile error and no test
failure. All verified against the checkout at `E:\GitHub\alp-sdk-vscode`.

- **`--version` first line must match `/^tan \d+\.\d+\.\d+/`**
  (`service.ts:107-121` `isNativeTanVersionOutput`, used by
  `service.ts:128-132` `parseTanVersion` for the behind/ahead skew checks and by
  `vscodeAdapter.ts:846-861` `commandOnPath` to decide whether a PATH binary is
  a real `tan`). A Python `tan` whose `--version` prints anything else is
  treated as *not `tan`*.
- **The pin is `SUPPORTED_CLI_VERSION = "0.4.0"`** (`service.ts:27`). The
  extension resolves a **released tag** — `v${version}`. A change merged to
  tan-cli `dev` reaches nobody here until it is released *and* pinned.
- **Invocation**: `spawn(command, [...args, "--format", "json"], { cwd, signal })`
  (`adapterCore.ts:152-176`, seam at `vscodeAdapter.ts:784`). **No `shell: true`**,
  no `env` override — the child inherits `process.env`. Output cap
  `ALP_SPAWN_MAX_OUTPUT = 16 * 1024 * 1024`.
- **Envelope shape is asserted** (`service.ts:235-246` `isEnvelope`): `command`
  string, `ok` boolean, `exitCode` number, `issues` array. Parsed from trimmed
  stdout (`service.ts:219-233`); anything unparseable yields `null` and the
  extension degrades quietly. **Nothing but JSON may reach stdout.**
- **Issue codes are matched by exact string.** Reproduce these verbatim:
  `"bootstrap.prerequisites-missing"`, `"bootstrap.python-not-runnable"`,
  `"bootstrap.python-too-old"` (`service.ts:397-401` `PREREQ_CODES`), plus
  `"bootstrap.windows-unsupported"` and `"bootstrap.yocto-host"`
  (`service.ts:365-386` `bootstrapHostVerdict`), all with `severity: "error"`.
  Note these already encode host-Python prerequisite handling — the Python port
  must keep the distinction between *the interpreter that runs `tan`* (bundled,
  invisible) and *the host west/Zephyr Python* these codes describe.
- **Startup overhead is not free.** `vscodeAdapter.ts:762` sets
  `ALP_SPAWN_TIMEOUT_MS = 60_000`, and `vscodeAdapter.ts:288-290` probes
  `--version` with a **3 s** timeout. PyInstaller one-file pays a per-invocation
  unpack cost; the `--version` probe is the tightest budget in the system and
  must be measured against that 3 s. **Verify which spawn path a long build
  actually uses before assuming the 60 s cap does not apply to it** — that is an
  explicit MVP check, not an assumption.

## Test strategy

- **Unit** (pytest) on `core/`: `apply_env_append` de-dup, `assemble_slice_env`
  precedence, `resolve_action` across the policy matrix, token substitution,
  and the version-skew guard's reject paths. Pure functions, no IO.
- **Parity** (the oracle above): dry-run capture over the AEN + native_sim
  matrix, asserted against Rust `tan`.
- **End-to-end**: `tan build --native --sdk-root <alp-sdk> --board-yaml
  examples/multicore/rpmsg-v2n/board.yaml` must compile a real `zephyr.elf` +
  `zephyr.bin`, matching the verified Rust e2e.
- **Packaging**: the PyInstaller artifact runs the e2e on a machine without a
  development Python install of `tan`.

## Error handling

Failures stay on the **value path**, not exceptions escaping to the top level —
every failure becomes an envelope with `ok: false`, a non-zero `exitCode`, and
structured `issues`. The existing exit-code mapping is preserved verbatim; the
extension parses these, so any divergence is an envelope-parity failure caught
by the oracle. Plan-level rejections (bad `schemaVersion`, unknown required
key, non-tokened `planPathMode`) fail loudly *before* any slice runs.

Plans remain **trusted input**, as today: writes are confined under
`buildRoot`, and tools/interpreters are resolved by explicit path.

## Risks

| Risk | Mitigation |
|---|---|
| ~~Python artifact can't be hash-pinned → vscode security regression~~ **RETIRED** | No hash or signature verification exists in the extension today (`download.ts:11-16`), so there is nothing to regress from. The real constraint is *shape*: PyInstaller `--onefile`, because the download path has no unpack step. |
| PyInstaller startup cost breaks the 3 s `--version` probe | Measured as an explicit acceptance criterion against `vscodeAdapter.ts:288-290`; one-file unpack cost is the known worst case. |
| Reverse drift while porting, no reference | Rust `tan` is the live oracle for every capability; it is retired only after parity. |
| Process/cancel/streaming UX regression on long builds | stdlib `subprocess`; west is the existence proof. Cancellation + streaming are explicit acceptance criteria, not assumed. |
| Python environment ambiguity (documented drift trap in this lab) | The shipped artifact bundles its own interpreter; the west venv is resolved explicitly and separately. Never PATH. |
| Startup overhead vs a Rust binary | Measured and recorded in the packaging deliverable. |

## Acceptance criteria

Sub-project 1 is done when all five hold:

1. `tan build` and `tan build --native` run in Python end-to-end on AEN +
   native_sim, producing a real `zephyr.elf` + `zephyr.bin`.
2. Envelope parity with Rust `tan` on every exercised path, success and failure.
3. Execution parity (command / env / skip-fail decision) over the AEN +
   native_sim dry-run matrix.
4. A PyInstaller `--onefile` artifact under the existing asset names, dropped
   into the extension's download path **with zero extension changes**, running a
   real build end-to-end; `--version` prints `tan X.Y.Z` as its first line and
   the probe completes inside the 3 s budget.
5. Unit tests green on `core/`; the repo's existing cargo gates still green
   (the Rust workspace is untouched).

## What "shippable" means — the two acceptance targets

Set by the maintainer 2026-07-29. These are the definition of done for the whole
port, not smoke tests. Byte-parity against the Rust binary is evidence; **these
two are the product.**

### Target 1 — a fresh customer, blink or RGB LED

A customer who has never used the SDK installs `tan`, scaffolds a blink /
RGB-LED project, builds it, flashes it, and the LED blinks on real hardware.
Nothing else. If that path has a rough edge, the port is not shippable.

The path exercises `tan init` → template scaffold → build → flash, and it runs
straight through the **known blocker** recorded under the CMakeLists survey
below: `scripts/alp_template.py::_scaffold_cmakelists()` rewrites a scaffolded
project's `CMakeLists.txt` by matching the *current* boilerplate text. A
customer scaffold that ships an unresolvable `include()` fails at this exact
target. Any CMake centralization must therefore land **with** the template
renderer, never before it.

Success is a blinking LED on the E1M-AEN801, verified on the bench — not a
successful compile — **and it must survive a cold power-cycle.** (Maintainer
decision, 2026-07-29. A RAM-run blink was explicitly considered and rejected:
customers ship devices that boot on their own, so a volatile demo would flatter
the result.)

> **Target 1 is currently blocked by an external dependency this port cannot
> resolve.** Persistent flashing of an E1M-AEN801 requires the Alif SETOOLS
> `app-gen-toc` to sign an ATOC, for every probe and every rebuild — and in this
> tree SETOOLS is **Linux-only**: `scripts/west_commands/runners/alif_flash.py:283`
> hard-codes the bundle name `app-release-exec-linux`. Three independent
> blockers, none of them fixable in Python:
>
> 1. `zephyr/boards/alp/e1m_aen801_m55_he/board.cmake:53-54` wires
>    `board_set_flasher_ifnset(alif_flash)`, so `tan flash` → `west flash` →
>    `alif_flash`, which hard-errors without both `SETOOLS_DIR` and `SE_UART`.
> 2. `docs/debugging-aen.md:79-81`: *"the J-Link write itself is SETOOLS-free,
>    but producing a valid, signed `AppTocPackage.bin` is not. There is no
>    'stock J-Link, no SETOOLS' flashing path."*
> 3. SETOOLS is Linux-only (above), while `docs/cross-platform-setup.md:887-888`
>    promises customers the opposite — a separate, customer-facing doc defect.
>
> **MCUboot slot1 is not the way out** — investigated and rejected under three
> adversarial lenses. **OpenOCD and CMSIS-DAP are not options either**:
> `openocd_config` is populated **zero** times across all of `metadata/`; only
> `jlink_flash_device` exists, for one part. "Probe-only" means J-Link
> specifically.
>
> **The one open question, and the experiment that settles it.** Four places in
> the repo *state* that the SE content-verifies slot0; one place *records* the
> SE booting **"a garbage image"** from a blank slot0
> (`CHANGELOG.md:2988-2993`). An SE that boots garbage did not verify the bytes
> it booted — behaviour outweighs prose, so this is genuinely undecided rather
> than merely undocumented. The test: on a sacrificial module, J-Link `loadbin`
> a **one-byte-modified** copy of the resident `zephyr.bin` to `0x80010000`,
> then **rail the board down and back up** (not `SYSRESETREQ`). If it boots, the
> SE does not content-verify slot0 and a probe-only model becomes architecturally
> live. `Verify successful.` must NOT be used as the pass signal —
> `docs/debugging-aen.md:71` records that exact signal lying. **Parked by the
> maintainer until the port is finished.**

**The artifact is `examples/peripheral-io/gpio-button-led`, not
`examples/aen/aen-pwm-utimer-pwmleds`.** Both drive the EVK's RGB cluster, but
only one is the customer path:

| | `gpio-button-led` | `aen-pwm-utimer-pwmleds` |
|---|---|---|
| `board.yaml` | yes — `som.sku: E1M-AEN801`, `preset: e1m-evk` | **none** |
| Scaffoldable | yes — canonical source for the **`peripheral`** template | no — not in the catalog |
| Exercises the planner | yes — `board.yaml` → `alp_project.py` → `EXTRA_CONF_FILE` | **no** — plain `prj.conf` app |
| Purpose | customer teaching example | *"on-silicon (scopeless) validation of `zephyr/drivers/pwm/pwm_alif_utimer.c`"* |

`gpio-button-led`'s own comment records why it is the RGB path on this board:
the EVK *"routes the user button to the encoder push switch (`E1M_GPIO_IO4`) and
its only user LEDs to the RGB cluster on PWM"*.

`aen-pwm-utimer-pwmleds` remains useful as an independent bench sanity check —
it proves the LEDs physically light — but it validates a driver, not the toolchain
under port.

**Consequence:** Target 1's critical path runs **through** the
`_scaffold_cmakelists()` blocker, because `gpio-button-led` is one of the eight
templates that function regex-rewrites. The fresh-customer path and the known
blocker are the same code path, not adjacent risks.

### Target 2 — an existing user upgrades

Someone on the shipped Rust `tan` v0.4.0 upgrades to the Python `tan` and their
existing project keeps building with **no manual migration**. Specifically:

- the VS Code extension's resolution/download path finds and runs the new binary
  (`SUPPORTED_CLI_VERSION`, the cached-binary replacement at
  `<globalStorage>/cli/tan[.exe]`);
- an **already-configured build dir** keeps working — this is what
  `sdk_stamp_action` / the `.tan-sdk-root` stamp exist for, and an upgrade is
  precisely the scenario that wipes-or-keeps a stale CMake cache;
- no plan re-emit, no config edit, no cache clear is demanded of the user.

An upgrade that silently produces a wrong image is worse than one that refuses;
the version-skew and `sdkCommit` guards are what make refusal the failure mode.

### What these targets imply for sequencing

Target 1 pulls `init` / scaffold / template rendering **out of sub-project 3 and
into the critical path** — they were scheduled as "the rest of the surface", but
a fresh customer cannot start without them. Target 2 makes the vscode
resolution path and the stamp logic first-class rather than cutover details.

## Boundaries the rest of the port must not cross (ADR-0017 unification)

alp-sdk is a **unification layer over the vendor SDK**, for hardware and
software alike. Moving the planner into `tan` must leave that position intact.
Two consequences bind sub-projects 2-4:

1. **`metadata/**` stays in alp-sdk. It does not move.** ADR-0017's rule is that
   every hardware fact (peripheral count, memory size, clock id, pin route)
   lives **once** under `metadata/**`, and every downstream file — `soc_caps.h`,
   `cap.h`/`cap.c`, board `dts`/`yaml`/`_defconfig`, route headers — is
   *generated* from it and carries a "do not edit — regenerate" banner. What
   relocates into `tan` is the **generators**, never the facts. `tan` reads
   `metadata/` and emits; alp-sdk remains metadata + C/HAL + the portable
   `<alp/*>` API. A hardware fact duplicated into the `tan` repo is a defect by
   this doctrine, not a convenience.

2. **Sub-project 2 is a RELOCATION, not a rewrite.** ADR-0017's other rule is
   "consume what exists; don't reimplement". The alp-sdk planner is *already
   Python* (~6,230 lines under `scripts/alp_orchestrate/`, plus `scripts/alp_cli/`),
   so it moves substantially as-is. Only the **Rust executor** is genuinely
   rewritten. Anyone treating sub-project 2 as a from-scratch reimplementation
   is both taking on needless risk and violating the doctrine — the planner's
   accumulated silicon knowledge is exactly what must not be re-derived.

The same rule applies to this port's own dependencies: consume `kconfiglib`,
`west`, `PyYAML`, `jsonschema` — never vendor or reimplement them.

## Correction: Zephyr's kconfig hook cannot be pointed at the `tan` binary

An earlier working assumption — that `--emit kconfig` could escape Python by
pointing `EXTRA_KCONFIG_TARGET_COMMAND_FOR_*` at the `tan` binary — is **false**,
verified against the Zephyr checkout:

- `zephyr/cmake/modules/kconfig.cmake:236-237` places `${PYTHON_EXECUTABLE}`
  immediately before the `EXTRA_KCONFIG_TARGET_COMMAND_FOR_<target>` expansion in
  one flat argument list. The value's first token is therefore always argv[0]
  **to the interpreter** and must be parseable Python source. `tan;kconfig-dump;…`
  would run `python tan kconfig-dump …` and fail.
- The alternate escape — overriding `PYTHON_EXECUTABLE` itself — is closed by
  `zephyr/cmake/modules/python.cmake:44`, a plain
  `set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})` that clobbers any `-D` override.
  This is the same trap already documented in `scripts/kconfig/alp_kconfig_dump.py`'s
  own docstring.

**Consequence for the goal.** "No Python *files* in the alp-sdk tree" is
achievable: the dumper shim can be generated into a `mkdtemp` scratch dir at
runtime, the same pattern `_load_board_symbols` already uses for its stub app.
"No Python *involved* in an alp-sdk build" is **not** achievable, and never was —
Zephyr and west are themselves Python, which is precisely why ADR-0020 refused a
Rust planner. Any plan that promises otherwise is promising something the build
system cannot deliver.

## The example-CMakeLists centralization (sub-project 4) — surveyed 2026-07-29

Removing `scripts/alp_project.py` from the tree requires dealing with the
example `CMakeLists.txt` files that shell it at CMake configure time. Survey
results, correcting two earlier claims:

- **96 files invoke it, not 124.** A `grep -rl` returns 124, but **28 are
  comment-only false positives** — `examples/aen/*-regcheck` and plain-`prj.conf`
  apps whose comments say they *do not* invoke it.
- **Six mechanical shapes**, not one: `A1`/`A2`/`A3` (72 files) differ only in
  `FATAL_ERROR` text; `B1`/`B2` (20 files) hardcode a relative
  `../../../scripts/alp_project.py` with **no `ALP_SDK_ROOT` override possible**
  and **no `OUTPUT_VARIABLE`/`ERROR_VARIABLE`, so stderr is lost on failure**;
  `C` (5 multicore files) makes a *second* `execute_process` for
  `ipc-contract-h`. Only two `--emit` modes appear anywhere: `zephyr-conf` (96)
  and `ipc-contract-h` (5). `--core` is always a literal, never derived.

**Ordering is load-bearing and already documented in the examples themselves.**
`execute_process` must run *before* `find_package(Zephyr)` because Zephyr pins a
Python interpreter the moment it imports, and `EXTRA_CONF_FILE` must be appended
before that same call. `prj.conf` cannot pull the fragment in itself because
`rsource` is a Kconfig directive and is **not valid in `.conf` files**. Any
helper that reorders `find_package(Python3)` → `execute_process` →
`list(APPEND EXTRA_CONF_FILE …)` → `find_package(Zephyr)` breaks configure
silently.

### The blocker that must be solved in lockstep

`scripts/alp_template.py::_scaffold_cmakelists()` **regex-rewrites these
CMakeLists when `tan init` scaffolds a customer project**, and it matches the two
*current* shapes by text. Eight catalog templates use these files as canonical
sources (`minimal`, `peripheral`, `sensor`, `multicore-rpmsg`, `gateway`,
`edge-ai`, `diagnostics`, `iot`). A third shape — `include(cmake/alp.cmake)` —
falls through that function's `return text` no-op path, so **every scaffolded
customer project would ship an `include()` naming a path that only resolves
inside an SDK checkout.** That ships broken projects to customers. The template
renderer must be updated in the same change, or the centralization must not
land.

### Teaching-value trade-off

A representative block is 49 lines: 20 comment, 6 blank, 23 mechanism. Only
**two** comments carry non-obvious knowledge — why a copied-out example needs
`ALP_SDK_ROOT`, and the `rsource`-is-invalid-in-`.conf` fact. Those two must
survive into the helper's own documentation or they are lost from the examples,
which are teaching artifacts here. The rest is boilerplate whose removal is a
net gain.

### Style precedent

`cmake/alp-sdk-warnings.cmake` is the only existing shared module, and **no
example includes any `cmake/*.cmake` today** — this would be the first. Its
conventions: `function()` (not `macro()`), `alp_sdk_*` snake_case names, a
doc-comment block above each definition, `_alp_sdk_*`-prefixed locals, guarded
early `return()`s.

## Follow-ups deliberately deferred

- **ADR-0020's verification claim is factually wrong** and should be corrected
  when the superseding ADR is written — it describes pinned-hash/signature
  verification that was never implemented (`alplabai/tan-cli#7` is still open).
  Whatever language `tan` is written in, the download is unverified today.
- **Asset names keep Rust target triples** (`tan-x86_64-unknown-linux-musl`
  etc.) so the MVP needs no extension change. The `musl` names imply a
  static-linking property PyInstaller does not provide; renaming them touches
  `service.ts:34-46` and belongs in the cutover sub-project.
- **Actually adding checksum verification** is orthogonal to this port and
  cheaper to do once, on whichever artifact ships.
