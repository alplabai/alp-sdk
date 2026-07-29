# The tan Python port — master plan

Date: 2026-07-29
Status: Draft — sub-project 1 nearly complete; 2-4 planned here
Decider: alpCaner

Supersedes the sequencing in
[2026-07-29-tan-python-executor-mvp-design.md](2026-07-29-tan-python-executor-mvp-design.md),
which stays authoritative for sub-project 1's detail. Read that first for the
two acceptance targets, the ADR-0017 unification guard, and the plugin-pattern
decision; this document is the plan across all four sub-projects.

## The goal, in one paragraph

`tan` becomes the sole planner and executor, written in Python, shipped as a
standalone single-file binary. alp-sdk keeps C/HAL, `metadata/**`, examples and
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
> *"That means your day-1 path is the normal Zephyr one — **no hand-run SETOOLS,
> no SE-UART wiring of your own**"*

The day-1 and rebuild loop is just:

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> \
    --sysbuild -- -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
west flash
```

What is true, and what is not:

| Claim | Verdict |
|---|---|
| Customer runs `app-gen-toc` / wires an SE-UART by hand | **False** — only for re-keying to a production key, or recovering a wiped module (`:80-85`) |
| SETOOLS must be *installed on the host* | **True, for BOTH flows** — `aen-provisioning.md:29-31`: *"both require SETOOLS' `app-gen-toc` to **sign** the ATOC first — Flow D runs `app-gen-toc` locally, with no SE-UART involved in that step"* |
| That host must be Linux/WSL2 | **True** — `alif_flash.py:283` hard-codes the bundle name `app-release-exec-linux` |

So the residual constraint is a **one-time install on a Linux host**, not a
per-rebuild burden.

**Target 1 is NOT blocked** (maintainer, 2026-07-29: *"the customer can download
setools from Alif"*). SETOOLS comes from Alif's developer download
(`app-release-exec-linux-SE_FW_x.y.z`, `aen-provisioning.md:91-94`) — a normal
one-time step in embedded, not something engineering can or should remove.
Earlier drafts of this plan called Target 1 "blocked by an external dependency
this port cannot resolve"; that was overstated and is withdrawn.

What the port actually owes here — all tractable:

1. **`tan doctor` must detect SETOOLS and guide.** Today neither `alp doctor`
   nor `tan doctor` mentions `SETOOLS_DIR`, `SE_UART` or the required `fdt` pip
   package at all, so a customer gets a clean bill of health and then fails at
   flash time with a raw `RuntimeError`. The check must name the Alif download
   and the exact env var.
2. **Fix the false cross-platform promise.** `docs/cross-platform-setup.md:887-888`
   tells customers *"If you're targeting an M-class core only, you never need to
   leave macOS or Windows"* — untrue while `alif_flash.py:283` hard-codes
   `app-release-exec-linux`. Either state the Linux/WSL2 requirement plainly, or
   make the Windows/macOS SETOOLS bundle work.
3. **Default to Flow D** (below), so the customer never wires an SE-UART.

### Flow D is what we use for MRAM — and `tan flash` should default to it

Maintainer, 2026-07-29: *"we use flow D for MRAM."* The two host paths
(`aen-provisioning.md:33-46`):

| | Flow A | **Flow D** |
|---|---|---|
| Transport | SETOOLS over the **SE-UART** | **J-Link direct over SWD** |
| Needs `app-gen-toc` | yes | **yes** — run locally |
| Needs an SE-UART | **yes** — a 1.8 V-capable USB-UART, which the docs call *"the #1 trap"* (`:95`) | **no** |
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
  (`aen-bench-bringup.md:195`)
- bench-verified 2026-06-17; *"Flow D is the day-to-day default now"*
  (`aen-bench-bringup.md:72`, `:244`, `:252`)

**Port decision:** `tan flash` should default to **Flow D** for AEN MRAM, not
inherit `west flash`'s Flow A default. That is a real UX gain the port can
deliver — a fast SWD burn with no SE-UART wiring — and it is already the
validated bench path. It requires a Flow-D flash backend under the runner
abstraction, with Flow A retained as the fallback and for re-keying/recovery.

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
- the issue codes in `contract/issue-codes.json` — 27 of them, 5 frozen, and
  `bootstrap.windows-unsupported` **retired**, its spelling never to be reused
- the 8 release asset names

That makes `contract/envelopes/` (15 fixtures) and `contract/issue-codes.json`
**the specification** for sub-project 3, not merely a regression net. A fixture
that fails is a port defect by definition; there is no "we improved it" defence.

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
| Port target version is **v0.5.0** | `python/tan/version.py`, commit `370c1ea` |
| **Standalone `tan` binary** is the customer entry point — not a west extension | maintainer, 2026-07-29 |
| **Plugin pattern**: tan = generic core; alp-sdk ships commands + runners into it | spec, commit `92fc4f6e` |
| **ONE** alp-sdk extension, unified command surface; only *runners* are per-vendor | spec, commit `c103d4ae` |
| **`tan` must never learn a hardware fact** — no SKU, address, pin or vendor branch | spec, commit `c103d4ae` |
| `metadata/**` stays in alp-sdk; generators move, facts do not | ADR-0017; spec `6301d754` |
| Target 1 requires **persistence** — a RAM-run blink does not count | maintainer, 2026-07-29 |
| Renode is retired | maintainer, 2026-07-29 |
| Rust's open bugs are **fixed** during the port, not replicated | maintainer, 2026-07-29 |
| PyInstaller `--onefile`; measured 0.500 s startup, 10,237,542 B | Task 7 |

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

## Distribution: BOTH pip and a single-file binary (decided 2026-07-29)

Maintainer: *"tan should be installable with pip too."* Verified working in a
clean Python 3.12 venv — `pip install .` then `tan --version` → `tan 0.5.0-dev`,
envelope path intact. `[project.scripts] tan = "tan.__main__:main"` already
provides it; the `requires-python = ">=3.12"` floor correctly refuses a 3.11
interpreter with `ERROR: Package 'tan' requires a different Python`.

| Path | For whom | Notes |
|---|---|---|
| **`pip install tan`** | anyone who already has Python ≥3.12 — developers, CI, container images | No platform binaries needed at all. Sidesteps the 8-asset / glibc-floor / musl problem entirely for these users. |
| **PyInstaller `--onefile`** | the bare-machine bootstrap case, and the VS Code extension's download path | Mandatory there: you cannot pip-install the tool whose job is to install Python. The extension writes a raw binary to one cached path with no unpack step. |

Neither replaces the other. **pip is the better default for the customer who
already has Python**, which is most embedded developers, and it makes the
frozen-binary distribution problem smaller rather than solving it: the binary is
still required for bootstrap and for the extension.

Consequence for the 8-asset contract: the unresolved `-musl` / glibc-2.31 /
cross-compile problem now blocks a *narrower* audience than it appeared to. It
still must be settled before cutover, but "pip for developers, binary for
bootstrap" is a viable interim.

## The one unresolved mechanism

**How does a frozen `tan` load an alp-sdk extension?** west imports a module
from a path named in a YAML. A PyInstaller `--onefile` binary carries its own
CPython, so importing the customer's alp-sdk Python crosses an interpreter
boundary. Candidate mechanisms, to be decided by the coupling map:

- **A — subprocess with a JSON contract.** `tan` invokes alp-sdk Python as a
  child process. **This is what already happens today** (`python -m alp_cli
  <sub>`), so it is proven and needs no interpreter gymnastics. Costs a process
  per call and requires a host Python.
- **B — in-process import from the customer's environment.** Cheapest at
  runtime, but the frozen CPython must import third-party code built for a
  *different* interpreter. Likely fatal; must be tested, not assumed.
- **C — declarative-only extensions.** alp-sdk ships manifests (YAML/JSON), not
  code; `tan` interprets them. Strongest for longevity — no ABI at all — and it
  is what the build-plan already is. Cannot express arbitrary logic.
- **D — hybrid: C for the common case, A where real code is needed.** Most
  likely answer, and the shape the repo already leans toward.

**Nothing in sub-project 2 starts until this is settled with evidence.**

## Sub-project sequencing

### Sub-project 1 — executor MVP *(nearly complete)*

Tasks 1-8 of
[the MVP plan](../plans/2026-07-29-tan-python-executor-mvp.md). Delivers the
plan parser, token substitution, materialise, execute+cancel, the contract-golden
harness, `--onefile` packaging and the Rust-oracle parity harness. **Deliberately
not a CLI** — `tan build` is still unregistered.

Exit criteria: all 8 reviewed clean; branch pushed.

### Sub-project 2 — the extension mechanism + a real CLI

The architecture work, and the first point at which `tan` becomes usable.

1. **Settle the loading mechanism** (above) with a spike, not a document.
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

Driven by the 15 contract fixtures, which are currently **15/15 xfail** and are
the real backlog. `build` has **no** fixture coverage at all
(`contract/README.md` says so), so it needs its own tests.

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

- **The 96 example `CMakeLists.txt`** → one `cmake/alp.cmake` helper, landed
  **with** `_scaffold_cmakelists()`. 96 real invokers, not 124; six mechanical
  shapes; ordering (`find_package(Python3)` → `execute_process` →
  `EXTRA_CONF_FILE` → `find_package(Zephyr)`) is load-bearing and documented in
  the examples themselves.
- **Distribution — unresolved.** The release contract is **8** assets. PyInstaller
  cannot produce the `-musl` pair (static, any libc) at all, cannot hold the
  `-gnu` pair's glibc 2.31 floor, and cannot cross-compile. Decide: native
  runner per target / drop `-musl` / keep a Rust shim.
- **`SUPPORTED_CLI_VERSION` → 0.5.0** in alp-sdk-vscode, or the release reaches
  nobody. `isCliAhead` is consulted only for a PATH-resolved binary, so nothing
  warns on skew by itself.
- Repoint alp-sdk CI, `bootstrap.sh`/`.ps1`, `doctor`, ~25 docs. Retire renode.
  Retire the Rust crates last, and only for capabilities the parity harness has
  confirmed.
- Supersede ADR-0020 with an ADR recording the plugin architecture — and correct
  its false "pinned-hash / signature verification" claim, which
  `alp-sdk-vscode/src/alpCli/download.ts:11-16` disclaims in its own words.

## Cross-cutting work not owned by any sub-project

| Item | Why it matters |
|---|---|
| **No CI runs `python/tests/` at all** | Every green count in this port is local-only. A Python CI leg is owed before any of it is trustworthy. |
| **Python frozen-issue-codes gate** | `contract/issue-codes.json` has 27 codes (5 frozen, 1 retired). The extension matches by exact `===` and **fails open** — a wrong code is silent with CI green on both sides. Must also handle tan-cli **#224** (the emit-site gate is blind to codes built by a prefixing helper). |
| **The Alif SETOOLS licensing question** | Target 1's real blocker. Sequence in parallel; no engineering unblocks it. |
| **`docs/cross-platform-setup.md:887-888`** | Tells customers "you never need to leave macOS or Windows" — false for the lead part, since SETOOLS is Linux-only. Customer-facing defect. |

## What would change this plan

- If the loading-mechanism spike shows the frozen binary **cannot** load
  extensions in-process, sub-project 2 keeps its shape but becomes
  subprocess-and-manifest driven (mechanism A/C/D).
- If the SETOOLS/ATOC experiment ever runs and shows the SE does **not**
  content-verify slot0, Target 1's blocker dissolves and probe-only flashing
  becomes the customer path.
- If distribution cannot produce 8 honest assets, the cutover keeps a Rust shim
  and `tan` ships hybrid for longer than planned.
