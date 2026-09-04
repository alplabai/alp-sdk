# The tan Python port — master plan

Date: 2026-07-29 (drafted); reconciled 2026-08-05 (alp-sdk#1194)
Status: Sub-project 1 SHIPPED — `tan` v0.5.0 is the released PyInstaller
freeze of the Python package `alp-tan` (verified against
`alplabai/tan-cli@0277b4c`, `python/pyproject.toml:12,17,35`). Renode's
alp-sdk-side doc/ADR record is retired ([ADR 0022](../../adr/0022-python-executor-renode-retirement.md)),
but the `tan renode` command itself is **not yet removed** — `python/tan/cli.py`
still registers `app.command("renode")` in `alplabai/tan-cli@0277b4c`; removal
is tracked as `tan-cli`#448, scheduled after the v0.5.0 tags (see Sub-project 4
below). Distribution is decided, not open (below). Sub-projects 2-4 remain the
open work.

**Provenance note (2026-08-05):** this file was carried over from
`design/tan-python-purge` (`ac3ae8be`), which is not part of this branch's
history, to reconcile it per alp-sdk#1194. Its two sibling documents,
[`2026-07-29-tan-python-executor-mvp-design.md`](2026-07-29-tan-python-executor-mvp-design.md)
and [`../plans/2026-07-29-tan-python-executor-mvp.md`](../plans/2026-07-29-tan-python-executor-mvp.md),
still live only on that branch and are not carried over by this pass — the
links below are dead until/unless those land too. Read this document for the
now-corrected distribution/mechanism/status facts; do not follow the two
sibling links expecting them to resolve here.

Supersedes the sequencing in
[2026-07-29-tan-python-executor-mvp-design.md](2026-07-29-tan-python-executor-mvp-design.md)
(design-branch only, see provenance note — **this link is currently dead in
this branch's tree**), which stays authoritative for sub-project 1's detail
once it lands. It covers the two acceptance targets, the ADR-0017 unification
guard, and the plugin-pattern decision; this document is the plan across all
four sub-projects.

## The goal, in one paragraph

`tan` becomes the sole planner and executor, written in Python, shipped as a
standalone binary — a PyInstaller `--onedir` archive per platform, not a
single-file binary (see "Distribution" below for why `--onefile` was
dropped). alp-sdk keeps C/HAL, `metadata/**`, examples and
firmware — and ships its command surface and flash backends **into** `tan` as
extensions, the way Zephyr extends west. Success is not "the port compiles"; it
is the two acceptance targets: a fresh customer scaffolds a blink project and
sees the LED blink on an E1M-AEN801 (surviving a cold power-cycle), and an
existing Rust-`tan` v0.4.0 user upgrades with no manual migration.

## CORRECTION — the AEN developer loop already works; only the SETOOLS *install* is the constraint

Earlier drafts of this plan claimed Target 1 was blocked because "a customer
cannot flash an E1M-AEN801 without SETOOLS". **That conflated two different
things and overstated the problem.** `docs/aen-provisioning.md:58-85` is
explicit:

> *"**E1M-AEN modules ship pre-provisioned by Alp Lab.** At manufacturing we
> write a development-signed **MCUboot** bootloader as the factory ATOC and a
> small **self-test** image into MCUboot's primary slot (slot0), with the module
> left in lifecycle state **DM** (development — debug open, fully
> re-provisionable)."*
>
> *"That means your day-1 path needs **no hand-run SETOOLS and no SE-UART
> wiring of your own** — two proven ways to get your app into MCUboot's
> slot0"*

The day-1 and rebuild loop is just:

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> \
    --sysbuild -- -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
west flash
```

What is true, and what is not:

| Claim | Verdict |
|---|---|
| Customer runs `app-gen-toc` / wires an SE-UART by hand | **False** — only for re-keying to a production key, or recovering a wiped module (`:120-124`) |
| SETOOLS must be *installed on the host* | **True, for BOTH flows** — `aen-provisioning.md:29-31`: *"both require SETOOLS' `app-gen-toc` to **sign** the ATOC first — Flow D runs `app-gen-toc` locally, with no SE-UART involved in that step"* |
| That host must be Linux/WSL2 to hand-run SETOOLS | **True, but not because alp-sdk's code hard-codes it** — Alif publishes only a Linux SETOOLS bundle (`app-release-exec-linux-SE_FW_x.y.z`, `aen-provisioning.md:131-132`). `scripts/west_commands/runners/alif_flash.py` has no platform gate anywhere in the file (verified: no `sys.platform`/`os.name` check) and its `--se-uart` help text at `:337` explicitly contemplates macOS device paths; `app-release-exec-linux` appears only inside argparse help/error strings (`:279,318,325`), never as a hard-coded branch. The Linux requirement is external (Alif's own distribution), not a constraint alp-sdk's runner code imposes — and it applies only to the rare hand-run SETOOLS path (re-keying/recovery), not to Flow D. |

So the residual constraint is a **one-time install on a Linux host, for the
re-keying/recovery path only** — not a per-rebuild burden, and not something
the default Flow D flow needs at all.

**Target 1 is NOT blocked** (maintainer, 2026-07-29: *"the customer can download
setools from Alif"*). SETOOLS comes from Alif's developer download
(`app-release-exec-linux-SE_FW_x.y.z`, `aen-provisioning.md:131-132`) — a normal
one-time step in embedded, not something engineering can or should remove.
Earlier drafts of this plan called Target 1 "blocked by an external dependency
this port cannot resolve"; that was overstated and is withdrawn.

What the port owed here — all shipped, verified against `alplabai/tan-cli@0277b4c`:

1. **`tan doctor` must detect SETOOLS and guide — SHIPPED.**
   `python/tan/commands/doctor_cmd.py:1058-1131` checks `$SETOOLS_DIR`,
   `$SE_UART`, and whether the `fdt` package is importable, and prints the
   exact Alif download name plus the env vars to set when any is missing
   (`python/tan/core/setools.py` drives the actual `app-gen-toc` signing step
   once SETOOLS is resolved).
2. **The cross-platform promise is accurate as stated, for the default path.**
   `docs/cross-platform-setup.md:912-913` (*"If you're targeting an M-class
   core only, you never need to leave macOS or Windows"*) holds for the
   day-1/day-to-day loop, because that loop uses Flow D (J-Link over SWD, no
   SETOOLS transport dependency beyond the one-time host install covered by
   item 1) — see the runner analysis above. The promise does not cover the
   rare re-keying/recovery path, which still needs a Linux host for hand-run
   SETOOLS; that narrower fact is not currently called out next to the
   cross-platform promise and is worth a one-line footnote there, but it is
   not the customer-facing defect earlier drafts of this plan described.
3. **Default to Flow D — SHIPPED**, see below.

### Flow D is what we use for MRAM — and `tan flash` should default to it

Maintainer, 2026-07-29: *"we use flow D for MRAM."* The two host paths
(`aen-provisioning.md:33-46`):

| | Flow A | **Flow D** |
|---|---|---|
| Transport | SETOOLS over the **SE-UART** | **J-Link direct over SWD** |
| Needs `app-gen-toc` | yes | **yes** — run locally |
| Needs an SE-UART | **yes** — a 1.8 V-capable USB-UART, which the docs call *"the #1 trap"* (`:135`) | **no** |
| Persists to MRAM | yes | **yes** — *"it also persists to MRAM, same as Flow A"* |
| Speed | — | **~0.16 s** burn |
| Used by `west flash` | **yes, by default** (`board_set_flasher_ifnset(alif_flash)`) | **no** |

**Flow D eliminates the SE-UART, not SETOOLS.** That matters because the SE-UART
is the genuinely painful dependency — dedicated 1.8 V hardware, separate wiring,
and the documented #1 trap — whereas `app-gen-toc` is a host binary that runs
locally and invisibly.

Flow D requirements, verbatim and not to be rounded off:
- the **part-number device profile** `AE822FA0E5597LS0_M55_HE`, *not* generic
  `Cortex-M55` — the generic profile has no MRAM loader
- J-Link **V9.46+** DLL (bench has V9.50), probe on matched **J-Link V13**
  firmware
- the **MRAM-XIP build shape**, not the ITCM (Flow C) build
  (`aen-bench-bringup.md:406`)
- bench-verified 2026-06-17; *"Flow D is the day-to-day default now"*
  (`aen-bench-bringup.md:82`)

### STATUS — arming Flow D shipped end to end, in both repos (reconciled 2026-08-06)

Earlier drafts of this plan (and, briefly, this reconciliation pass) treated
arming Flow D as open work — first "a one-function change on the alp-sdk
side", then a "two-repo change" still to be done. **Both were wrong: the
whole path is shipped**, verified directly against source, not against a
stale docstring (see the note at the end of this section):

- **The device profile is plan-time metadata, and it is already threaded
  through.** `metadata/socs/alif/ensemble/e8.json` `variants[1].debug.
  jlink_flash_device` = `AE822FA0E5597LS0_M55_HE`. alp-sdk's
  `scripts/alp_orchestrate/loader.py:182-232` (`_resolve_jlink_flash_device`)
  resolves it from the SoC spec, `models.py:153` carries it as a field on
  `Slice` (not a separate `soc_spec` parameter — the threading problem this
  plan worried about doesn't exist because the fact is resolved once, at
  `Slice` construction, in `loader.py`), and `orchestrator.py:65-73`'s
  `_slice_flash_recipe` adds `flash_args["jlink_flash_device"]` whenever the
  field is set. `Slice.to_manifest_entry` (`models.py:163`) and
  `buildplan.py:445`'s `_slice_flash_recipe(slice_)` call both already
  receive a fully-resolved `Slice` — no extra context-threading was needed.
  `tests/fixtures/emit-snapshots/{rpmsg-aen,mproc-mailbox}.system-manifest.snap`
  emit `jlink_flash_device: AE822FA0E5597LS0_M55_HE` today, and
  `tests/scripts/test_orchestrate_manifest.py::
  test_emit_system_manifest_aen_flash_args_carries_jlink_flash_device` pins it.
- **The MRAM and ATOC addresses are NOT plan-time facts, and tan reads them
  from the build artefact, not from metadata — as this plan always intended.**
  `app-gen-toc` writes them into `build/app-package-map.txt` at *signing*
  time; the bench scripts parse them with
  `awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1`
  (`scripts/bench/aen/flash-jlink-mramxip.sh:154`, `flash-jlink-hp.sh:89`,
  `flash-jlink.sh:97`), and `tan-cli`'s own
  `python/tan/core/flash_plan.py::parse_atoc_start_address` parses the same
  marker byte-for-byte from the same file. No MRAM or ATOC address exists
  under `metadata/` — correctly.
- **`tan flash` already defaults to Flow D.**
  `python/tan/core/flash_plan.py::select_flash_method` picks
  `alif_mram_jlink` (Flow D) whenever a `zephyr_west_flash` entry's
  `flash_args` carries every key in `FLOW_D_KEYS` (`("jlink_flash_device",)`)
  — Flow A (`zephyr_west_flash` as-is) is the fallback when the key is
  absent, exactly the design this plan called for. `flash_cmd.py:1049` calls
  it. `python/tan/core/setools.py` drives `app-gen-toc` automatically given a
  resolved `$SETOOLS_DIR`, so a customer with SETOOLS installed does not
  hand-run it even for the one-time signing step.

**A note on how this plan got it wrong twice.** `tan-cli`'s own
`flash_plan.py::select_flash_method` docstring still says, as of
`alplabai/tan-cli@0277b4c`: *"with today's emit
(`tan/planner/orchestrator.py::_slice_flash_recipe` returns
`("zephyr_west_flash", {})` for every Zephyr slice) NO entry carries that
key, so every AEN slice still takes Flow A. Arming Flow D is now a
one-function change in THIS repo."* That comment is itself stale —
`python/tan/planner/orchestrator.py:65-73` (tan-cli's own relocated copy of
the alp-sdk orchestrator) already carries the `jlink_flash_device` branch,
identical to alp-sdk's. The earlier draft of this section was written from
that stale tan-cli comment rather than from reading the actual code on both
sides; this reconciliation pass corrects the record by reading the code, not
by trusting either repo's prose. Any future doc pass touching
`tan-cli`'s `flash_plan.py` should also fix that stale docstring so the next
reader doesn't repeat the mistake.

This also retires the MCUboot-slot1 investigation for the right reason: **the
shipped design already uses MCUboot as the factory ATOC.** The earlier analysis
concluded "MCUboot slot1 is dead" because it was asked whether we could *invent*
a SETOOLS-free path — the wrong question, since the mechanism already exists and
is what modules ship with.

**Dependency ownership** (maintainer, 2026-07-29): the VS Code extension manages
dependency installation incrementally — west, Zephyr, the SDK and toolchains.
Python is assumed rather than installed by us. SETOOLS is the one dependency we
cannot install (license-gated) but must detect and explain.

## INVARIANT — one `board.yaml`, and the OS is DERIVED, never chosen

Maintainer, 2026-07-29, marked *"you need to keep those things"*:

> *"we define everything from one file `board.yaml` and yocto and zephyr are
> automatically selected for the cores… for example core M33, zephyr, for A32
> yocto is selected automatically."*

**This is the product, not an implementation detail.** A customer writes exactly
one `board.yaml`. They never pick an OS, never pick a build system, never pick a
toolchain. The per-core OS class is **derived** from the core's architecture —
Cortex-M → Zephyr, Cortex-A → Yocto — and everything downstream (which tool
runs, which config is emitted, which artefacts appear) follows from that
derivation. The taxonomy lives in `scripts/alp_orchestrate/topology.py`
(`core_os_topology()` / `emit_os_topology()`) and the derived value reaches the
executor as `slices[].backend`.

Binding consequences for the port:

1. **`tan` never offers an OS/backend choice.** No `--os`, no `--backend`, no
   prompt, no default to override. If a command appears to need one, the
   derivation is incomplete — fix it in the planner, do not add a flag. A flag
   here would convert a derived fact into a second source of truth and break
   both the unification guard and this invariant at once.
2. **The derivation stays in alp-sdk**, with `metadata/**`. `tan` receives an
   already-resolved `backend` per slice and executes it. `tan` must not contain
   the Cortex-M→Zephyr / Cortex-A→Yocto mapping, or any core-to-OS knowledge at
   all — that is a hardware fact.
3. **One `board.yaml` drives every slice.** A heterogeneous build (M33 + A55 in
   one module) comes from a single file and a single `tan build`. Splitting
   per-OS inputs, or requiring one invocation per OS class, regresses the
   product even if every individual command still works.
4. **This is a review gate**, alongside the two unification rules: any change
   that makes the OS user-selectable, duplicates the taxonomy into `tan`, or
   requires more than one input file is rejected regardless of its other merits.

Prior art to preserve, not rediscover: this was already settled once — the
recorded decision is that OS is *class-derived, not a picker*, and
`system-manifest.yaml` is the contract carrying the result.

## The user-facing surface is FROZEN; the internals are ours

Maintainer, 2026-07-29: *"the tan commands will stay as is and tan will be
used. rest is up to you."*

**What is frozen** — port it, do not redesign it:

- every command name and its arguments, exactly as the Rust `tan` has them
- the envelope shape `{command, ok, exitCode, project, sdk?, data, issues}`,
  with `sdk` **absent** when unresolved, never `null`
- the six exit codes (`0` Success … `5` InternalFailure)
- the issue codes in `contract/issue-codes.json` — measured at
  `alplabai/tan-cli@0277b4c`: 280 entries (278 unique codes), 274 `reserved`,
  5 `frozen`, and 1 `retired` (`bootstrap.windows-unsupported`, its spelling
  never to be reused)
- the release asset names — **this draft-time bullet said 8; the shipped,
  measured contract is 4** (see "Distribution" below). Frozen means "port
  the shipped contract verbatim", not "port whatever an earlier draft
  guessed"; treat this bullet as corrected to 4, not as a second live answer
  alongside Distribution's 4.

That makes `contract/envelopes/` (17 fixtures, measured) and
`contract/issue-codes.json` **the specification** for sub-project 3, not
merely a regression net. A fixture that fails is a port defect by
definition; there is no "we improved it" defence.

**What is ours** — the internal architecture: the backend abstraction, how
extensions are registered and loaded, module layout, where the planner lives,
how runners are dispatched. The unification guard still binds every one of
those choices (one unified surface; `tan` never learns a hardware fact).

The one deliberate exception, already agreed: **Rust's open bugs are fixed, not
replicated.** Where a fixture encodes buggy behaviour, fix the bug and update
the fixture in the same change, citing the issue.

## Decisions already taken (do not re-litigate)

| Decision | Where |
|---|---|
| Port target version was **v0.5.0**; **shipped** as `alp-tan` `0.5.0` | `python/pyproject.toml:12,17` (`alplabai/tan-cli@0277b4c`) |
| **Standalone `tan` binary** is the customer entry point — not a west extension | maintainer, 2026-07-29; shipped |
| **Plugin pattern**: tan = generic core; alp-sdk ships commands + runners into it | spec, commit `92fc4f6e` |
| **ONE** alp-sdk extension, unified command surface; only *runners* are per-vendor | spec, commit `c103d4ae` |
| **`tan` must never learn a hardware fact** — no SKU, address, pin or vendor branch | spec, commit `c103d4ae` |
| `metadata/**` stays in alp-sdk; generators move, facts do not | ADR-0017; spec `6301d754` |
| Target 1 requires **persistence** — a RAM-run blink does not count | maintainer, 2026-07-29 |
| Renode is retired (the `tan renode` verb; alp-sdk's own Renode CI is untouched) | maintainer, 2026-08-04; [ADR 0022](../../adr/0022-python-executor-renode-retirement.md) |
| Rust's open bugs are **fixed** during the port, not replicated | maintainer, 2026-07-29 |
| Distribution is **4 GitHub-release assets**, PyInstaller **`--onedir`** (not `--onefile` — dropped for a macOS startup regression, `tan-cli`#349); PyPI publication **not enabled** | `alplabai/tan-cli` `.github/workflows/release.yml`; `docs/cli.md:30,46-47`; see "Distribution" section below |

## west/Zephyr is a reference for the MECHANISM, not for the SCOPE

**Zephyr is MCU-only. Alp Lab is not.** A single `board.yaml` describes
Cortex-M cores running Zephyr *and* Cortex-A cores running Linux/Yocto, on the
same module — the build-plan's `slices[].backend` enum is
`["zephyr", "yocto", "baremetal"]`, and each is executed by a different tool
(`west`, `bitbake`, `cmake`). West cannot drive bitbake and never will.

Two consequences, both load-bearing:

1. **It confirms the standalone-binary decision for a stronger reason than
   coverage.** A west extension is structurally incapable of being the entry
   point for a heterogeneous build, because west is *Zephyr's* tool. `tan` must
   sit **above** west, not inside it. (The weaker justification — "a Yocto-only
   customer has no west" — is true but secondary.)
2. **The extension point is per-BACKEND, not per-Zephyr.**
   `ZephyrBinaryRunner` is the right shape for one OS class only. `tan` needs
   the tier above it: a **backend abstraction** where `zephyr`→west,
   `yocto`→bitbake and `baremetal`→cmake all satisfy one interface, with
   probe/runner plugins living *underneath* the zephyr backend. Copying
   Zephyr's runner registry directly into `tan` would bake an MCU-shaped
   assumption into the core and strand the A-core path.

This is also the sharpest statement of what unification is worth here: **Zephyr
unifies across MCU boards; alp-sdk unifies across MCU *and* MPU, and across
three silicon vendors (Alif / Renesas / NXP).** That span is what `<alp/*>` plus
`metadata/**` provides and no vendor SDK does — and it is precisely what a
second source of truth inside `tan` would destroy.

See ADR-0010 (heterogeneous OS orchestration) for the existing decision this
builds on.

## Distribution: RESOLVED — 4 GitHub-release assets; PyPI not enabled (reconciled 2026-08-05)

**This section previously held two contradictory answers** (a "decided: BOTH
pip and a single-file binary" text here, and a "distribution — unresolved,
keep a Rust shim" text under Sub-project 4). Neither survives contact with
what shipped. The distribution is settled by measurement, not by re-opening
the question:

- **The install name is `alp-tan`, not `tan`** — `tan` is unrelated on PyPI
  (`python/pyproject.toml:12`, verified `alplabai/tan-cli@0277b4c`), currently
  at **`version = "0.5.0"`** (`python/pyproject.toml:17`), `requires-python
  = ">=3.12"` (`:35`). `[project.scripts] tan = "tan.__main__:main"`
  (`python/pyproject.toml:168`) means an environment that already has the
  package installed still runs the command as `tan`.
- **PyPI publication is NOT enabled.** `alp-tan` is a reserved distribution
  name only; nothing is published to PyPI today (`docs/cli.md:46-47`:
  *"`alp-tan` is reserved as the distribution name but is not published on
  PyPI yet"*). `pip install alp-tan` does not work against the real index.
  `pip install <path-to-python/>` (a local/git checkout) works for
  development, and is what `docs/cli.md`'s own install instructions use.
- **The release contract is 4 assets, not 8** (`alplabai/tan-cli`
  `.github/workflows/release.yml:56-59,289,298,301,306`, verified):

  | Asset | Target |
  |---|---|
  | `tan-x86_64-pc-windows-msvc.zip` | Windows x64 |
  | `tan-x86_64-apple-darwin.tar.gz` | macOS x64 |
  | `tan-aarch64-apple-darwin.tar.gz` | macOS arm64 |
  | `tan-x86_64-unknown-linux-gnu.tar.gz` | Linux x64 |

  Each is a **PyInstaller `--onedir`** freeze (`release.yml:37-40`;
  **not** `--onefile` — `--onefile` was tried and dropped, `tan-cli`#349,
  for a 13-19s macOS startup regression it caused, `release.yml:637-639`).
  The Linux asset is deliberately `-gnu`, not `-musl`: it is frozen in
  `python:3.12-slim-bullseye` (Debian 11, **glibc 2.31**, `release.yml:308`),
  so it genuinely is a glibc binary and an honest name for it.
- **Two alternatives were measured and rejected**, not left open:
  - *musl/Alpine* — PyInstaller's musllinux bootloader links
    `/lib/ld-musl-x86_64.so.1` and runs **only on musl distros**; it is not
    the "any libc" artefact a `-musl` name implies (`release.yml:80-86`).
  - *manylinux2014* — ships a static-only CPython with no shared
    `libpython3.12*.so`; PyInstaller refuses outright without one
    (`release.yml:86`).
- **arm64 Windows and arm64 Linux are deferred, not blocked.** No asset
  ships for either today — an accepted scope decision for CI-minutes cost on
  a public repo, not a missing-runner problem: `windows-11-arm` and
  `ubuntu-24.04-arm` are current hosted runner labels
  (`release.yml:65-68,271`). Revisit whenever arm64 assets are wanted; a
  source install already covers that gap (`release.yml:652-655`).
- **No Rust shim survives cutover.** The "loading mechanism" section below
  explains why the concern that motivated keeping a shim doesn't apply to
  the shipped design.

Consequence for downstream docs: any doc still describing an "8-asset
contract", a `-musl` asset, or `pip install tan` is describing a plan that
did not ship, not a bug to be reproduced.

## The loading mechanism — RESOLVED: relocate the planner, don't load an extension (reconciled 2026-08-05)

**Original framing (2026-07-29):** how does a frozen `tan` load an alp-sdk
extension, given a PyInstaller binary carries its own CPython and importing
the customer's alp-sdk Python crosses an interpreter boundary? Four
candidates (A subprocess+JSON, B in-process import of the customer's
environment, C declarative-only manifests, D hybrid) were listed, undecided.

**What shipped sidesteps the question rather than answering it as posed.**
Per `docs/cli.md`'s current description of Python Tan: *"Python Tan contains
the shipping planner and executor. Its relocated planner reads `board.yaml`
plus metadata and schemas from the selected alp-sdk checkout, produces a
build plan in process, and the executor materialises and runs that plan."*
The planner was **ported into `tan`'s own Python package**
(`python/tan/{commands,core,planner,templates}/` in `alplabai/tan-cli`), not
dynamically loaded from the customer's alp-sdk checkout at runtime — so
there is no cross-interpreter import for the default path at all. `tan`
reads alp-sdk's `metadata/**` and `board.yaml` as **data**, in process, the
same way it always read a build-plan JSON; it does not import alp-sdk's
Python.

This is closest to candidate **C** (declarative data, not loaded code) for
the default path, with candidate **A** (subprocess) kept as an explicit,
non-default compatibility/parity fallback: `docs/cli.md` — *"`tan generate`
renders its supported targets from the relocated planner in process and uses
the SDK subprocess only as an explicit or reported compatibility
fallback"* and *"`TAN_GENERATE_EXECUTOR=subprocess` explicitly selects the
reference `scripts/alp_project.py` engine for parity/debugging. In automatic
mode, an unavailable in-process engine falls back with a visible
`generate.in-process-unavailable` warning ... the fallback is never
silent."* alp-sdk's original planner and emitters remain in place as the
canonical reference and parity oracle while the port settles
(`docs/cli.md` "Where planning and commands live").

Candidate B (importing the customer's *installed* alp-sdk Python in-process)
was not needed and is not what shipped — relocating the planner's logic
avoided the interpreter-boundary problem it would have hit.

## Sub-project sequencing

### Sub-project 1 — executor MVP *(SHIPPED — reconciled 2026-08-05)*

Tasks 1-8 of
[the MVP plan](../plans/2026-07-29-tan-python-executor-mvp.md) (design-branch
only — **this link is currently dead in this branch's tree**, see provenance
note above). Delivered the plan parser, token
substitution, materialise, execute+cancel, the contract-golden harness, and
the Rust-oracle parity harness. Packaging shipped as PyInstaller
**`--onedir`**, not the `--onefile` this plan originally specified (dropped,
`tan-cli`#349 — see "Distribution" below). **No longer "not a CLI"** — `tan
build` and the full command surface below are registered and released in
`tan` v0.5.0; the `crates/` Rust workspace is now frozen as the
behaviour-oracle used by the parity harness, not the development path.

Exit criteria met; superseded by shipped v0.5.0.

### Sub-project 2 — the extension mechanism + a real CLI

**Status (reconciled 2026-08-05): shipped.** `tan` v0.5.0 registers a full
command surface (`docs/cli.md` lists 32 verbs) and item 1 below is resolved
(see "The loading mechanism" above) — kept here as a historical task list,
not open work.

The architecture work, and the first point at which `tan` becomes usable.

1. ~~**Settle the loading mechanism** (above) with a spike, not a document.~~
   Resolved — see above.
2. **Register a real command surface.** Today `python -m tan build` exits 2 with
   empty stdout, and bare `tan` exits **0 silently** where Rust exits 2 with help
   (found by Task 8) — fix both.
3. **The extension contract**: the registration manifest, discovery, and how a
   command's envelope reaches stdout unchanged.
4. **A runner base class in `tan`**, with alp-sdk's flash backends as the first
   implementations — mirroring `ZephyrBinaryRunner`.
5. **Wire `sdk_stamp_action`** (ported in Task 2, never called) — and fix
   tan-cli **#183** (`--pristine` is a silent no-op when either wipe guard
   suppresses it) rather than reproducing it. This is Target 2's stale-build-dir
   path.

### Sub-project 3 — the command surface

**Status (reconciled 2026-08-05): shipped, superseding the "15/15 xfail"
snapshot below.** Per `docs/cli.md`, most of `tan`'s verbs — including
`doctor`, `init`, `validate`, `generate`, `model`, `new-som`, `faultdecode`,
`monitor`, `run`, and the build/flash/image/simulation surface — are native
Python implementations; only `migrate`, `lock`, and `quality` still forward
to `west alp-*`. The task list below is kept as the original backlog
snapshot, not current status.

Driven by the 15 contract fixtures, which were **15/15 xfail** at draft
time and were the real backlog then. `build` had **no** fixture coverage at
all (`contract/README.md` said so then), so it needed its own tests.

Order by acceptance target, not by convenience:

- **Target 1 critical path:** `init` / scaffold / template rendering →
  `build` → `flash`. Note `_scaffold_cmakelists()` regex-rewrites the
  scaffolded `CMakeLists.txt`, so any CMake change lands **with** the renderer
  or fresh customers get an unresolvable `include()`.
- **Target 2 critical path:** `doctor`, `validate`, and the manifest/state I/O
  an existing project already has on disk.
- **Everything else:** `image`, `size`, `clean`, `sdk`, `presets`, `pinmux`,
  `explain`, `inspect`, `trace`, `debug-config`, `support-bundle`, `completion`.

Fold in the open-issue triage (task #12) — **#222** (a `'TBD'` placeholder is
truthy and reaches a real flasher) is a hardware-consequence bug and must not
survive the port.

### Sub-project 4 — cutover

- **The example `CMakeLists.txt` files** → one `cmake/alp.cmake` helper, landed
  **with** `_scaffold_cmakelists()`. Measured counts (neither the draft's "96"
  nor "124" is reproducible from the tree, so recorded here directly):
  168 total `examples/**/CMakeLists.txt`, 125 of which reference
  `alp_project.py` or `alp.cmake` (the real invokers the helper migrates),
  158 call `find_package(Zephyr...)`, 102 use `execute_process`, 98 set
  `EXTRA_CONF_FILE`. Ordering (`find_package(Python3)` → `execute_process` →
  `EXTRA_CONF_FILE` → `find_package(Zephyr)`) is load-bearing and documented in
  the examples themselves. Whoever executes this cutover should re-run the
  count against `dev` at execution time rather than trust this snapshot.
- **Distribution — RESOLVED**, not open; see the "Distribution" section
  above (4 assets, `--onedir`, PyPI not enabled, no Rust shim). This bullet
  previously duplicated a now-corrected contradiction — kept as a pointer so
  a reader landing here directly doesn't re-open it.
- **`SUPPORTED_CLI_VERSION` → 0.5.0** in alp-sdk-vscode, or the release reaches
  nobody. `isCliAhead` is consulted only for a PATH-resolved binary, so nothing
  warns on skew by itself. **Not independently verified in this reconciliation
  pass** — check `alp-sdk-vscode`'s current pin before relying on this line.
- **Renode is retired** — done, not open work. [ADR 0022](../../adr/0022-python-executor-renode-retirement.md)
  covers the alp-sdk-side doc/ADR record; `tan-cli`#448 covers the `tan
  renode` command removal, scheduled after the `tan` v0.5.0 tags. Retire the
  Rust crates last, and only for capabilities the parity harness has
  confirmed — done: `tan-cli`'s `2883cdf4` (2026-08-10) deleted `crates/`
  and the oracle-parity suite outright (#269, #601); `git ls-tree
  origin/dev -- crates` on `tan-cli` is empty.
- **Repoint alp-sdk CI, `bootstrap.sh`/`.ps1`, `doctor`, and the following
  docs** (the original note said "~25 docs" without naming them; this
  reconciliation pass verified the ones below against current `dev` and
  found most already repointed — treat this as the enumerated checklist the
  vague count owed, not as an exhaustive audit of every doc that mentions
  `tan`):

  | Doc | Status verified 2026-08-05 |
  |---|---|
  | `docs/cli.md` | Already describes Python `tan`, the 4-asset/`--onedir` release, and the `alp-tan`/PyPI-not-yet fact |
  | `docs/adr/0020-sdk-owns-build-execution.md` | Append-only ADR; correctly flagged as partially superseded rather than rewritten |
  | `docs/adr/0022-python-executor-renode-retirement.md` | Already records the Python-not-Rust fact and the Renode-verb retirement |
  | `docs/adr/README.md` | Already annotates 0020 as "partially superseded by 0022" |
  | `docs/cross-platform-setup.md` | Already distinguishes `python -m alp_cli doctor` (SDK reference) from `tan doctor` |
  | `docs/board-config-schema.md`, `docs/glossary.md`, `docs/portability-matrix.md`, `docs/troubleshooting.md`, `docs/diagnostics/ALP-B004.md`, `docs/test-plan.md` | Each already scopes its `python -m alp_cli` mention to alp-sdk's own reference tooling, not `tan` |
  | `README.md` | **Not verified / likely still stale** — `docs/adr/0022`'s own "costs" section names `README.md`'s `cargo install` / Rust-toolchain paragraphs as known-stale and explicitly out of that ADR's bounded scope. `README.md` is off-limits to this reconciliation pass (shared file); flagged for the owning unit. |
  | `docs/getting-started.md` | Out of this reconciliation's file scope (owned by a sibling wave-3 unit) |
  | Remaining docs matching `tan\b` under `docs/` (~35 hits at last count) | Not individually re-verified in this pass; most mention `tan` only as the build-tool name and carry no language/distribution claim to go stale |
- **Partially done.** [ADR 0022](../../adr/0022-python-executor-renode-retirement.md)
  supersedes ADR-0020 narrowly — the Rust-executor language clause and every
  `renode` command-surface entry only (`docs/adr/README.md`'s ADR-0020 row).
  ADR-0020's plans-vs-executes split, three-repo boundary, and contract are
  otherwise still the live record. **Still open:** an ADR recording the
  plugin/extension architecture itself (the "relocate the planner" shape
  resolved above), and correcting ADR-0020's "pinned-hash / signature
  verification" claim — **not independently re-verified against
  `alp-sdk-vscode/src/alpCli/download.ts:11-16` in this pass**; carried over
  from the original plan text.

## Cross-cutting work not owned by any sub-project

| Item | Why it matters |
|---|---|
| **No CI runs `python/tests/` at all** | Every green count in this port is local-only. A Python CI leg is owed before any of it is trustworthy. |
| **Python frozen-issue-codes gate** | `contract/issue-codes.json` has 280 codes (278 unique: 274 reserved, 5 frozen, 1 retired), measured at `alplabai/tan-cli@0277b4c`. The extension matches by exact `===` and **fails open** — a wrong code is silent with CI green on both sides. Must also handle tan-cli **#224** (the emit-site gate is blind to codes built by a prefixing helper). |
| **The Alif SETOOLS one-time host install** | **Not a blocker — see the "CORRECTION" section above: Target 1 is NOT blocked.** Sequence the customer-facing SETOOLS-detection UX (shipped in `tan doctor`, see above) in parallel with everything else; no engineering removes the download step itself, but none is needed to unblock Target 1. |
| **`docs/cross-platform-setup.md:912-913`** | "You never need to leave macOS or Windows" for an M-class-only target — accurate for the default Flow D path (see the CORRECTION section above); worth a one-line footnote there scoping it away from the rare SETOOLS re-keying/recovery path, which does need Linux. Not the customer-facing defect earlier drafts described. |

## What would change this plan

- ~~If the loading-mechanism spike shows the frozen binary cannot load
  extensions in-process...~~ **Moot — resolved 2026-08-05.** The shipped
  design relocated the planner into `tan`'s own package instead of loading
  alp-sdk's Python at runtime, so this contingency never triggered; see "The
  loading mechanism" above.
- If the SETOOLS/ATOC experiment ever runs and shows the SE does **not**
  content-verify slot0, Target 1's blocker dissolves and probe-only flashing
  becomes the customer path. (Not evaluated in this reconciliation pass.)
- ~~If distribution cannot produce 8 honest assets, the cutover keeps a Rust
  shim...~~ **Moot — resolved 2026-08-05.** The shipped contract is 4 assets,
  measured and settled (musl and manylinux2014 both disqualified on
  measurement, not left pending); see "Distribution" above. No Rust shim.
