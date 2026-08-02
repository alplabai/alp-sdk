# The `tan` CLI

`tan` is the Alp SDK's sole user-facing command-line front door (ADR
[0020](adr/0020-sdk-owns-build-execution.md), end-state B): scaffolding
a project, inspecting the generated configuration, validating
`board.yaml`, compiling AI models, sanity-checking the host
environment, opening a serial console, decoding a diagnostic code or a
fault dump, scaffolding the metadata for porting a new SoM, and
building / flashing / sizing / bundling / cleaning / Renode-booting a
project.

`tan` is a standalone, independently-versioned, public Rust binary
([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)); install it
separately. The automatic installer needs no Rust toolchain:

```bash
curl -fsSL https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"  # install.sh already made this permanent in your shell rc; needed once more in THIS shell
```

Or grab a prebuilt, per-platform binary directly from a release tag, e.g.
Linux x86_64 — use the `-musl` asset, not `-gnu`: the `-gnu` build requires
`GLIBC_2.30`+ and hard-fails (`version 'GLIBC_2.30' not found`) on distros
older than roughly Ubuntu 20.04/Debian 11, while `-musl` is fully static and
runs on any distro/libc:

```bash
curl -fsSL -o tan \
  https://github.com/alplabai/tan-cli/releases/latest/download/tan-x86_64-unknown-linux-musl
chmod +x tan && sudo mv tan /usr/local/bin/tan
```

Building from source instead needs Rust 1.86+ (get it from
[rustup.rs](https://rustup.rs)) plus a system C toolchain
(`build-essential` on Debian/Ubuntu, `gcc`/`gcc-c++` on Fedora/RHEL -- see
[`docs/cross-platform-setup.md`](cross-platform-setup.md) §2.1):

```bash
git clone https://github.com/alplabai/tan-cli && cd tan-cli
cargo install --path crates/tan-cli --locked
```

None of the three install paths above pin a `tan` version (`install.sh`
tracks the mutable `main` ref and installs `releases/latest`; the
source path clones `main`'s HEAD) -- this is deliberate: all three are
tan-cli's own recommended install methods, and staying on its latest
release is the point until a project needs otherwise.

Two execution paths live behind the one binary:

* **Build / flash / size / image / clean / renode** -- `tan`'s own
  primary build executor.  Per ADR
  [0020](adr/0020-sdk-owns-build-execution.md), alp-sdk is
  **plans-only** for this surface: `tan` consumes
  `alp_orchestrate --emit build-plan` / `--emit system-manifest` and
  runs `west` / `bitbake` / `cmake` per slice itself.
* **Host preflight** -- `tan doctor` and `tan doctor --build` are
  native Rust checks implemented in tan-cli itself, not forwarded
  anywhere; their check lists are unrelated to alp-sdk's own separate
  Python preflight (`python -m alp_cli doctor`, aka "`alp doctor`" in
  older docs) -- see [below](#tan-doctor).
* **Everything else** (`init`, `new-som`, `validate`, `model`,
  `monitor`, `explain`, `faultdecode`, `generate`, `run`) --
  `tan` forwards to the SDK's Python backend, most verbs as `python -m
  alp_cli <sub>`; `tan generate` is the one exception, invoking
  `scripts/alp_project.py` directly rather than through `alp_cli` (see
  [below](#tan-generate)).
  No `alp` binary is installed anywhere -- `pyproject.toml`
  registers only `alp-mcp`; `alp_cli` is a library `tan` shells out to,
  never a user-installed command of its own.  The bootstrap scripts
  (`scripts/bootstrap.sh` on Linux/macOS/WSL2, `scripts/bootstrap.ps1`
  on native Windows) install `alp_cli` as an editable package into the
  workspace venv, so a `git pull` in the alp-sdk checkout updates the
  backend in place; it also runs straight from a checkout without
  `tan`, for backend debugging:

  ```bash
  # From the alp-sdk repo root (or with <sdk>/scripts on PYTHONPATH):
  PYTHONPATH=scripts python3 -m alp_cli --help
  ```

Every forwarded verb is a **thin wrapper**: the actual logic lives in
the same scripts the surviving `west alp-*` extension commands drive
(`scripts/alp_orchestrate/`, `scripts/alp_project.py`,
`scripts/validate_board_yaml.py`).  `tan` adds discoverability and
sane defaults; it never forks the behaviour.

## `tan` vs `west alp-*` -- which one do I use?

Two front doors, two different jobs -- pick by what you're doing:

| You are... | Use |
|---|---|
| Scaffolding a project, validating `board.yaml`, compiling a model, checking your host, opening a serial console, decoding a diagnostic/fault, or running a quick single-image native_sim/single-board loop | `tan init` / `tan new-som` / `tan validate` / `tan model` / `tan doctor --build` / `tan monitor` / `tan explain` / `tan faultdecode` / `tan run` |
| Inspecting a board-derived Zephyr/CMake/Yocto/DTS config artefact without building (`zephyr-conf`, `dts-overlay`, `cmake-args`, `yocto-conf`, `native-sim-overlay`, `carrier-netlist`) | `tan generate --target <mode>` |
| Inspecting an orchestrator-owned artefact without building (system manifest, build plan, Kconfig menu, IPC contract header, DTS reservations/partitions, storage mounts, TF-M sysbuild overlay) | `west alp-emit <mode>` from a west workspace |
| Building, flashing, sizing, bundling, cleaning, or Renode-booting a project | `tan build` / `tan flash` / `tan size` / `tan image` / `tan clean` / `tan renode` -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) |
| Scripting the surviving west-centric maintenance commands | `west alp-migrate` (board.yaml schema migration) / `west alp-lock` (dependency lockfile) / `west alp-quality` (quality-task registry) / `west alp-emit` (generated-artefact subset) |

Rules of thumb:

* `tan` is the SDK's sole user-facing front door for both the
  plan-based, multi-slice build surface and every scaffold / validate
  / inspect / host-tool verb (ADR
  [0020](adr/0020-sdk-owns-build-execution.md), end-state B).  The
  non-build verbs (`generate`, `validate`, `explain`, `faultdecode`,
  `init`, `new-som`, `monitor`, `model`, `run`) forward to the Python
  backend; alp-sdk itself never runs them directly.  `tan doctor` is
  the one exception -- a native Rust check, not a forwarded verb (see
  [below](#tan-doctor)).
* For build/flash: `tan` consumes the SDK's
  `alp_orchestrate --emit build-plan` (and seeds its own
  `system-manifest.yaml` / `.alp-build-state.json` from
  `--emit system-manifest`), then drives `west` / `bitbake` / `cmake`
  per slice, owns skip-vs-fail policy, and programs hardware.  `tan
  build` (`--native` is the default, explicit opt-in) materialises the
  plan and runs each slice's build command directly for the real SoM
  `board.yaml` targets; it never runs the produced binary itself
  (that's `tan run`).  See
  [heterogeneous-builds.md](heterogeneous-builds.md) for the per-core
  fan-out the plan describes.
* `tan generate` and `west alp-emit` cover **disjoint** artefact sets,
  not a superset/subset pair: `tan generate --target <mode>` reaches
  only the six board-derived config targets `alp_project.py` owns
  (`zephyr-conf`, `dts-overlay`, `native-sim-overlay`, `cmake-args`,
  `yocto-conf`, `carrier-netlist`); `west alp-emit <mode>` reaches only
  the eight orchestrator-owned targets (`system-manifest`,
  `ipc-contract-h`, `dts-reservations`, `dts-partitions`,
  `storage-mounts-c`, `tfm-sysbuild-conf`, `build-plan`, `kconfig`).
  Neither front door reaches `hw-info-h`, `west-libraries`,
  `composed-route-table`, `scaffold`, `zephyr-board`, or `os-topology`
  -- those remaining `alp_project.py` targets have no `tan` or `west`
  front door at all; reach them by running the SDK's own Python CLI
  directly (`python -m alp_cli emit <mode>`, which still exposes the
  full old catalog).  Same emitters underneath either way -- no front
  door can ever produce different output for the same mode.  Three of
  `alp_project.py`'s 15 modes (`system-manifest`, `dts-reservations`,
  `ipc-contract-h`) are the same artefacts `west alp-emit` already
  reaches -- `alp_project.py` just re-dispatches them to the same
  orchestrator emitters (see `emit.py`'s `PROJECT_EMIT_MODES` /
  `ORCHESTRATOR_EMIT_MODES` overlap). They are **deliberately
  orchestrator-internal**, not a `tan` gap: `tan` already gets them
  indirectly (`tan build` seeds `system-manifest.yaml` from `--emit
  system-manifest`; the other two feed the Zephyr build itself, not a
  user command). Of the six with no front door at all, `scaffold` and
  `composed-route-table` are also intentional, not gaps -- see
  "Six modes, no front door: which ones are gaps?" below. The other
  four (`hw-info-h`, `west-libraries`, `os-topology`, `zephyr-board`)
  have no `tan` companion and no design reason not to -- filed against
  `tan-cli` (see the same section).

## Verb reference

### `tan init` -- scaffold a new project

```bash
tan init --name my-app --som E1M-AEN801 --template minimal-app --sdk-root <path-to-alp-sdk> --non-interactive
tan init                     # interactive: prompts for every field
```

`tan init` takes options only -- there is no positional `NAME`
argument.  Scaffolds a new project directory (named by `--name`),
rewrites `board.yaml` for the chosen SoM SKU + template, and resolves
the alp-sdk checkout the project should build against via
`--sdk-root`.  Omit a flag to be prompted interactively, or add
`--non-interactive` (a global flag, not a `tan init` option -- see
`tan --help`) to fail instead of prompting, which is what CI wants.

| Option | Meaning |
|---|---|
| `--name` | New project directory name |
| `--som` | SoM SKU, e.g. `E1M-AEN801` |
| `--template` | Project template id, e.g. `minimal-app` (the default), `sensor-starter` |
| `--sdk-root` | Path to the alp-sdk checkout the scaffolded project should resolve |
| `--destination` | Where to write the project |
| `--cores` | Comma-separated `id[:os]` core map for a heterogeneous project (OS inferred from the id when omitted) |
| `--project`, `--board-yaml`, `--from-example`, `--target`, `--all`, `--format`, `--preview`, `--force`, `--quiet`, `--no-color`, `--verbose` | See `tan init --help` for the full reference -- not restated here to avoid a second copy drifting from the CLI. |

### `tan new-som` -- scaffold metadata for a new SoM port

```bash
tan new-som --sku E1M-NX9555 --soc-ref nxp:imx9:imx95 --family nxp-imx9
tan new-som               # interactive: prompts for every field
```

The vendor-N+1 porting kit.  Generates the two metadata skeletons a
new SoM port needs -- `metadata/e1m_modules/<SKU>.yaml` (the SoM
preset, canonical cross-family shape) and, when the target SoC has no
spec yet, `metadata/socs/<vendor>/<family>/<part>.json` -- with every
schema-required hardware-fact field present as an explicit `TBD`
placeholder (values are never invented; the JSON carries its TODOs in
the schema-sanctioned `_pending_reason` + `notes` fields).  Both
skeletons are schema-valid on arrival, and the command finishes with
the numbered porting checklist (fill TBDs, extend the schema `sku`
pattern for a brand-new family, register the silicon ref, validate,
regenerate, run the conformance suite).  The full walkthrough lives in
[porting-new-som.md](porting-new-som.md).

The scaffold is committable as-is: every input is validated *before*
anything is written (a rejected invocation never leaves half-written
files), `--default-board` must name a carrier from `metadata/boards/`,
and `--default-hw-rev` must resolve in the family's
`hw-revisions.yaml` whenever that file already exists (a brand-new
family defers this to the checklist).  The generated
`preferred_backend: tbd` placeholder passes
`scripts/check_inference_backend_parity.py` only while the preset
declares `status.preliminary: true` -- replace `tbd` with the real
silicon backend before clearing the flag.  Use `--dry-run` to see the
planned files (and run the full validation) without writing anything.
When stdin is not a terminal (CI), missing required flags fail fast
with an explicit list instead of dropping into the prompts.

| Option | Meaning |
|---|---|
| `--sku` | New SoM SKU, e.g. `E1M-NX9555` |
| `--soc-ref` | Silicon triple-colon ref, e.g. `nxp:imx9:imx95` |
| `--family` | Human-readable family slug, e.g. `nxp-imx9` |
| `--vendor` | Vendor display name for the SoC JSON (default: soc-ref vendor segment) |
| `--display-name` | Preset display name (default derived from the SKU) |
| `--inference-backend` | `ethos_u` / `drpai` / `deepx_dxm1` / `tbd` (default `tbd`) |
| `--ethos-u-variant` | `u55` / `u65` / `u85`; required with `--inference-backend ethos_u` |
| `--cores` | Comma-separated canonical core ids (default: a `tbd_core0` placeholder) |
| `--default-board` | Stock carrier board (default `E1M-EVK`) |
| `--default-hw-rev` | Default hardware revision (default `r1`) |
| `--output-root` | Root to generate `metadata/` under (default: the SDK checkout) |
| `--dry-run` | Validate and print the planned files; write nothing |
| `--force` | Overwrite an existing preset for this SKU |

Omit `--sku` / `--soc-ref` / `--family` to be prompted interactively
(requires a terminal; in a pipe or CI the command fails fast naming
the missing flags).

### `tan build` / `flash` / `size` / `image` / `clean` / `renode` -- build execution

Before ADR [0020](adr/0020-sdk-owns-build-execution.md) these were
`alp build` / `alp flash` / `alp size` / `alp image` / `alp clean` /
`alp renode` (and their `west alp-*` twins), fronted by the now-removed
`alp` console-script -- `tan run` is the forwarded single-image escape
hatch survivor (see the verb reference below).  ADR-0020 Phase 4
retired the SDK-side fan-out executor (`Orchestrator.fan_out()`,
`_dispatch_slice()`, and the
`west alp-{build,image,flash,clean,size,renode}` extensions).  The
multi-slice build/flash/size/image/clean/renode surface moved to the
standalone, public **`tan` CLI**:

```bash
tan build                          # materialise the plan + run every slice's build command (default)
tan --project my-app build         # same, for a project outside the cwd
tan build --plan                   # show the build plan only, no build
tan flash                          # program every slice + helper MCU
tan size --fail-over-budget        # footprint vs the SoM memory budget
tan image                          # assemble a flashable bundle
tan clean                          # remove build outputs
tan renode                         # headless smoke boot in Renode
```

`tan` consumes `alp_orchestrate --emit build-plan` (the machine-readable,
write-free build recipe -- one entry per non-`off` core, with the
resolved app source dir, the exact tool command, and env) as its only
input, and seeds its own `system-manifest.yaml` from
`--emit system-manifest`.  The plan is hermetic: under
`planPathMode: tokened`, checkout-/project-anchored absolute paths
are emitted as literal `${SDK_ROOT}` / `${PROJECT_ROOT}` / `${PYTHON}`
tokens instead of this run's real paths, and `tan` substitutes them
at materialise time -- see the "Hermetic paths" paragraph in
[heterogeneous-builds.md](heterogeneous-builds.md) for the field
list.  It is independently versioned and
distributed; installing it never pulls in alp-studio or the VS Code
extension.  See [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)
for its own verb reference (flags, `--dry-run`, `--sim-mode`, helper-MCU
targeting, and so on), and [ADR 0020](adr/0020-sdk-owns-build-execution.md)
for the plans-vs-executes split.

alp-sdk's own contribution to this pipeline is entirely inspectable
without `tan`:

```bash
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit build-plan
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit system-manifest
```

See [heterogeneous-builds.md](heterogeneous-builds.md) for the
per-core fan-out the plan describes.

### `tan generate` -- materialise a board-derived config artefact (no build) {#tan-generate}

```bash
tan generate --target zephyr-conf         # the per-core Zephyr fragment
tan generate --target dts-overlay         # board DTS overlay (bus aliases + pin array)
tan generate --target cmake-args          # per-core -D CMake argument list
tan generate --target yocto-conf          # per-core Yocto local.conf fragment
tan generate --target native-sim-overlay  # native_sim GPIO overlay
tan generate --target carrier-netlist     # Studio-facing carrier nets + BOM JSON
tan generate --all                        # every target above, one run
```

Writes files, not stdout -- there is no `--output` flag.  Each target
lands at a fixed, conventional path (`--force` to overwrite an existing
one):

| Target | Written to |
|---|---|
| `zephyr-conf` | `build/generated/alp.conf` |
| `dts-overlay` | `build/generated/alp.overlay` |
| `cmake-args` | `build/generated/alp-cmake-args.txt` |
| `yocto-conf` | `build/generated/alp-yocto.conf` |
| `native-sim-overlay` | `boards/native_sim_native_64.overlay` (the app's own source tree, not `build/`) |
| `carrier-netlist` | `build/generated/carrier-netlist.json` |

This replaces the retired `tan emit` command (a positional `tan emit
<mode>`, one flat catalog printed to stdout, an `--output` flag), but
it is a **narrower** catalog, not a rename: `tan generate --target`
only reaches the six `alp_project.py`-owned, per-core-config targets
above.  The rest of the old `tan emit` catalog has no `tan` front door
at all:

| Mode | Artefact | Owned by | Reachable via |
|---|---|---|---|
| `zephyr-conf` | Per-core Zephyr `alp.conf` Kconfig fragment | `alp_project.py` | `tan generate` |
| `cmake-args` | Per-core `-D` CMake argument list | `alp_project.py` | `tan generate` |
| `yocto-conf` | Per-core `local.conf` fragment | `alp_project.py` | `tan generate` |
| `dts-overlay` | Board DTS overlay (bus aliases + pin array) | `alp_project.py` | `tan generate` |
| `native-sim-overlay` | native_sim overlay: `alp,pin-array` on `zephyr,gpio-emul` | `alp_project.py` | `tan generate` |
| `carrier-netlist` | Studio-facing carrier nets + BOM JSON handoff | `alp_project.py` | `tan generate` |
| `system-manifest` | Full-system manifest (slices, boot order) | orchestrator | `west alp-emit` |
| `dts-reservations` | DTS reserved-memory overlay (cross-core carve-outs) | orchestrator | `west alp-emit` |
| `ipc-contract-h` | Cross-core IPC contract header | orchestrator | `west alp-emit` |
| `dts-partitions` | DTS fixed-partitions overlay (`storage:` entries) | orchestrator | `west alp-emit` |
| `storage-mounts-c` | Static C storage mount table | orchestrator | `west alp-emit` |
| `tfm-sysbuild-conf` | TF-M sysbuild child-image overlay (`security.psa:`) | orchestrator | `west alp-emit` |
| `build-plan` | Per-slice build plan, JSON (IDE / CI / `tan` consumers) | orchestrator | `west alp-emit` |
| `kconfig` | Board-scoped, user-settable Kconfig symbol menu for one `--core <id>` (the vscode `prj.conf` LSP's live feed) | orchestrator (**workspace-dependent** -- see below) | `west alp-emit`, `tan kconfig` |
| `hw-info-h` | Build-time `hw_info.h` macro header | `alp_project.py` | `python -m alp_cli emit` only |
| `west-libraries` | `west.yml` fragment for `libraries:` deps | `alp_project.py` | `python -m alp_cli emit` only |
| `composed-route-table` | JSON route-table dump (demonstrator) | `alp_project.py` | `python -m alp_cli emit` only |
| `scaffold` | New-project `{path, contents}[]` envelope for a template (`--template`/`--sku`) | `alp_project.py` | `python -m alp_cli emit` only |
| `zephyr-board` | Per-core Zephyr board tree (`--core` + `--output <dir>`) | `alp_project.py` | `python -m alp_cli emit` only |
| `os-topology` | Per-core natural-vs-effective OS facts | orchestrator, via `alp_project.py`'s dispatch shim | `python -m alp_cli emit` only |

`os-topology`'s logic still lives in the orchestrator (`emit_os_topology`),
but it is no longer one of `alp_orchestrate`'s own `--emit` choices
(`python -m alp_orchestrate --emit os-topology` now fails, exit 2,
"invalid choice") and was never in `west alp-emit`'s mode list either --
it is only dispatched through `alp_project.py`'s v2 shim, so
`python -m alp_cli emit os-topology` is its only surviving front door.
The six `python -m alp_cli emit`-only rows
have no `tan` or `west` front door at all; `python -m alp_cli emit
<mode>` still exposes the full old catalog end to end, including
`--output`, `--core`, `--template`, and `--sku` -- it is the SDK's own
unforwarded Python CLI, run straight from a checkout
(`PYTHONPATH=scripts python3 -m alp_cli emit --help`).

#### Six modes, no front door: which ones are gaps?

"No `tan` or `west` front door" isn't one category -- each of the six
was checked against its actual consumer (not guessed), per the emit
surface classification (ADR [0020](adr/0020-sdk-owns-build-execution.md)
amendment 6):

- **Gaps -- filed against `tan-cli`, no design reason for the absence:**
  `hw-info-h` and `west-libraries` are board-derived, per-project
  config artefacts exactly like the six `tan generate` already covers
  (`hw-info-h` bakes `board.yaml` identifiers into a header an app
  `#include`s and checks at runtime -- see the `hw-info-h` row in the
  emit-modes table above;
  `west-libraries` produces the `west.yml` fragment a project's own
  manifest imports) -- both are consumed by the same CI jobs
  (`cross-platform-zephyr.yml`, `pr-metadata-validate.yml`) as the six
  that DO have a `tan generate` target, with no reason the seventh and
  eighth don't. `os-topology` (per-core natural-vs-effective OS facts,
  issue [#95](https://github.com/alplabai/alp-sdk/issues/95)) is richer
  than `system-manifest`'s flat `slices[].os` -- it also carries
  `core_type`, `runtime_class`, `overridden`, and `allowed_os`, exactly
  the shape a heterogeneous-SoM configurator UI needs and
  `system-manifest` cannot provide -- but no `tan` or IDE consumer
  reads it yet (confirmed: zero hits for `os-topology`/`osTopology` in
  `alplabai/alp-sdk-vscode`). `zephyr-board` (per-core Zephyr board
  tree, issue #523) is the next documented step after `tan new-som` in
  the SoM-porting walkthrough ([porting-new-som.md](porting-new-som.md)
  §10) -- a real front door exists for scaffolding a new SoM's
  *metadata*, none for generating its *board tree*, so a porting
  partner falls off `tan` mid-workflow. All four filed:
  [`tan-cli`#113](https://github.com/alplabai/tan-cli/issues/113)
  (`hw-info-h`), [`tan-cli`#114](https://github.com/alplabai/tan-cli/issues/114)
  (`west-libraries`), [`tan-cli`#115](https://github.com/alplabai/tan-cli/issues/115)
  (`os-topology`), [`tan-cli`#116](https://github.com/alplabai/tan-cli/issues/116)
  (`zephyr-board`).
- **Intentional -- consumed by `tan` already, just not live:**
  `scaffold`'s output is vendored byte-for-byte into the `tan` binary
  at release time (`crates/tan-core/src/wizard/vendored/`,
  `include_str!`'d at compile time -- no filesystem or subprocess call
  at `tan init`/`tan scaffold` runtime), pinned to an SDK release tag
  and checked for drift by `tests/parity/scaffold_byte_parity.py`. It
  has no *live* front door by design (`tan init` is deliberately
  SDK-checkout-free). [`tan-cli`#14](https://github.com/alplabai/tan-cli/issues/14)
  tracks extending the vendored set to the wizard's remaining
  hand-written templates -- not a new gap, don't re-file it.
- **Intentional -- no product consumer, kept for its own value:**
  `composed-route-table` is explicitly a "demonstrator" in its own
  owning code (`scripts/alp_project_emit/bom_netlist.py`) and doc
  table (above) -- it exists to give
  `tests/scripts/test_emit_composed_route_table.py` a way to assert on
  the board × SoM pad-route composition algorithm's JSON shape, and it
  was run by hand once as a maintainer verification step during a
  hardware-revision fix (see `docs/portability-matrix.md`'s A2-1). No
  build, IDE, or `tan` verb consumes its output; it earns its keep as
  regression coverage for the composition logic, not as a shipped
  artefact. Not filed -- there's nothing to file.

| Option | Meaning |
|---|---|
| `--target` | Which of the six targets to generate |
| `--all` | Generate all six targets in one run |
| `--force` | Overwrite an existing output file |
| `--project`, `--board-yaml`, `--sdk-root`, `--format`, `--verbose`, `--quiet`, `--no-color`, `--non-interactive`, `--ci` | See `tan generate --help` for the full reference -- not restated here to avoid a second copy drifting from the CLI. |

`west alp-emit` exposes the orchestrator subset of the old catalog
(`system-manifest`, `ipc-contract-h`, `dts-reservations`,
`dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`,
`build-plan`, `kconfig`) for west-centric scripting.

#### `--emit kconfig` -- the SDK's first workspace-dependent emit

Every other `--emit` mode is hermetic (provable from `board.yaml` +
this repo's own metadata alone, with no Zephyr toolchain on disk --
see `scripts/check_emit_snapshots.py`, the byte-golden gate that pins
all of them). `--emit kconfig` is deliberately the first exception:
it needs a bootstrapped Zephyr workspace (`ZEPHYR_BASE`, the SDK's
pinned v4.4.1) because only the real Kconfig solver knows which
symbols are user-promptable for a given board -- the SDK doesn't (and
won't) re-implement Kconfig's dependency/visibility engine itself.

```bash
python -m alp_orchestrate --input board.yaml --emit kconfig --core m55_he
# also: west alp-emit kconfig --core m55_he
# also: tan kconfig --core m55_he         (its own top-level tan verb --
#                                           NOT tan generate, which has no
#                                           front door for this target)
```

Output shape:

```json
{
  "schemaVersion": 1,
  "board": "alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he",
  "core": "m55_he",
  "symbols": [
    { "name": "LOG", "type": "bool", "prompt": "Logging",
      "depends": "y", "default": "n", "help": "Enable logging." }
  ]
}
```

`name` is the bare symbol (the LSP prepends `CONFIG_`); `type` is one
of `bool`/`tristate`/`int`/`hex`/`string`; `symbols` is sorted by
`name` and scoped to symbols with a real Kconfig prompt (the ~few
hundred a customer could actually put in `prj.conf`, not the full
invisible ~26k-symbol tree). Without a bootstrapped `ZEPHYR_BASE` the
mode fails loudly (exit 2) rather than emit a partial/empty menu.

Because the mode needs a workspace, its regression coverage is split
the same way: `tests/scripts/test_emit_kconfig.py` unit-tests the
JSON-shaping hermetically (a fake symbol list, no Zephyr installed),
and `scripts/check_emit_kconfig_contract.py` -- a schema/smoke check,
not a byte-golden, since the exact symbol set moves with the pinned
Zephyr version -- runs against a real AEN core in the
Zephyr-bootstrapped `pr-twister` CI job (`.github/workflows/
pr-twister.yml`), never in the hermetic snapshot gate.

Landed in tan-cli [#35](https://github.com/alplabai/tan-cli/issues/35)
as the top-level `tan kconfig --core <id>` verb (wraps this emit in
`Envelope<KconfigData>` as planned; confirmed against the installed
`tan 0.3.1`).  A follow-up alp-sdk-vscode change to point the
`prj.conf` LSP's symbol menu at this live feed instead of its
hand-vendored snapshot is a separate repo's change, out of scope
here.

### `tan validate` -- check a board.yaml

```bash
tan validate                             # ./board.yaml, human output
tan validate path/to/board.yaml
tan validate --format json path/to/board.yaml    # IDE/LSP/CI-facing
tan validate --format sarif path/to/board.yaml   # SARIF 2.1.0 (code scanning)
```

Runs the rich diagnostic validator (JSON-Schema pass, SoM/preset
cross-references, peripheral-vs-SoC capability check), then the
same orchestrator consistency pass used by build preflight.
Exit code 0 means no hard errors; warnings such as ALP-B010 still
return 0.  Hard schema/xref/consistency errors return 1.

`--format` selects the rendering:

- `human` (default) -- the Rust-style block with an `ALP-Bxxx` code
  -- decode any code with `tan explain ALP-B001`.
- `json` -- the versioned machine document
  (`metadata/schemas/diagnostic-v1.schema.json`): `schemaVersion` is
  a version/capability handshake a consumer must check before
  parsing further, and every range is **zero-based** (LSP
  `Position`/`Range` convention).
- `sarif` -- a SARIF 2.1.0 log (`runs[].results[]`); SARIF regions
  are **one-based** by spec, the opposite of the `json` format's
  ranges -- the two exporters intentionally do not share range
  values.

`json`/`sarif` print only the structured document to stdout, no
interleaved human prose.

`scripts/validate_board_yaml.py` is a compatibility wrapper around
the same rich validator plus consistency pass, so `tan validate`,
the script entry point, and build preflight reject the same
board.yaml contracts.

### `tan model` -- compile, package, and pre-flight check AI models

```bash
tan model build                          # compile board.yaml `models:` entries
tan model build --board path/to/board.yaml --out build/models
tan model build --model demo             # build only the `demo` models: entry
```

Compiles every `models:` entry declared in `board.yaml` into a
`.alpmodel` package via the SoM-appropriate backend (Vela for
Ethos-U, DRP-AI for RZ/V2N, ...).  `--model NAME` restricts the build
to the single named entry instead of all of them; an unknown NAME
exits 1 instead of building everything.  See the model-pipeline docs
under `docs/tutorials/` for the end-to-end inference flow.

`tan model` fronts the whole model lifecycle -- `tan model build` /
`tan model list` / `tan model info` / `tan model doctor` (compile `board.yaml` `models:` into `.alpmodel`,
enumerate them, decode a built package, and report the compile
toolchains), `check` (static pre-flight fit/perf), `zoo` / `add`
(browse and pull curated model-zoo entries), and `prep` / `run` / `ab`
(license-free quantize + accuracy, host reference run, host A/B).  Each
`tan model <cmd>` mirrors the SDK backend's `alp model <cmd>` (invoked
here as `python -m alp_cli model <cmd>`); driven through tan-cli the
result is delivered in tan-cli's `{command, ok, exitCode, project,
data, issues}` envelope, and the `--format json` payloads below are the
model-specific `data` those envelopes carry.

#### `tan model list` / `tan model info` / `tan model doctor` -- inspect models + toolchains

```bash
tan model list                           # enumerate the board.yaml `models:` entries
tan model info <model.alpmodel>          # decode a compiled `.alpmodel` package
tan model doctor                         # report the model-compile toolchains present
```

`tan model list` enumerates the declared/compiled models, `tan model info` decodes a built
`.alpmodel` package, and `tan model doctor` reports which model-compile toolchains
(Vela, DRP-AI, ...) are available -- distinct from the host-wide `tan
doctor` preflight below.

#### `tan model check` -- static pre-flight fit/perf check (no toolchain)

Two modes:

```bash
# single-model: check one .tflite file against an explicit SKU
tan model check my_model.tflite --sku E1M-AEN801
tan model check my_model.tflite --sku E1M-AEN801 --format json
tan model check my_model.tflite --sku E1M-V2N101 --metadata-root path/to/metadata

# board-mode: check every (or one) board.yaml `models:` entry; SKU comes
# from `som.sku` unless overridden
tan model check --board path/to/board.yaml
tan model check --board path/to/board.yaml --model demo
tan model check --board path/to/board.yaml --sku E1M-V2N101 --format json
```

Offline, no-toolchain static analysis: parses the TFLite graph and,
for every backend the SKU exposes (`cpu` plus each on-SoM NPU),
answers "will this model fit, and roughly how fast" *before* any
compile.  Each backend gets a fit verdict -- `fits` / `cpu-fallback`
(one or more ops fall back to CPU) / `no-fit` (SRAM arena overflow) --
plus conservative `est_sram_kib` / `est_latency_ms` /
`op_coverage_pct` estimates.  MODEL and `--board` are mutually
exclusive.

In single-model mode `--sku` is required (e.g. `E1M-AEN801`). In
`--board` mode the SKU defaults to the board's `som.sku`; passing
`--sku` alongside `--board` overrides it. `--metadata-root` overrides
the SDK's own `metadata/` root (defaults to it) in either mode.

Exit 0 for any completed analysis -- a `no-fit` verdict is a valid
answer, not a CLI error. Exit 1 for a per-model analysis failure (an
unknown SKU, or a non-TFLite model -- ONNX static analysis is a
follow-on, not yet supported): in board-mode this is per-model --
one un-analysable `models:` entry does not abort the run, it's
recorded as an error entry and the whole invocation still exits 1
after printing the full payload. MODEL and `--board` together, or
neither MODEL nor `--sku` given in single-model mode, is a usage
error (exit 2); a missing/unreadable model path is likewise a usage
error (exit 2), same as any other `tan model` path argument.

Single-model `--format json` payload:

```json
{
  "model": "my_model.tflite",
  "sku": "E1M-AEN801",
  "backends": [
    {
      "backend": "ethos_u",
      "verdict": "fits",
      "est_sram_kib": 1,
      "budget_sram_kib": null,
      "est_latency_ms": 0.02,
      "op_coverage_pct": 100.0,
      "unsupported_ops": [],
      "source": "static"
    }
  ],
  "suggestion": null
}
```

Board-mode `--format json` payload -- one entry per `models:` entry
checked, each either a full result or (when that model's source isn't
analysable) a per-model `error`:

```json
{
  "board": "path/to/board.yaml",
  "sku": "E1M-AEN801",
  "models": [
    {
      "name": "demo",
      "source": "models/demo.tflite",
      "backends": [ { "backend": "ethos_u", "verdict": "fits", "...": "..." } ],
      "suggestion": null
    },
    {
      "name": "other",
      "source": "models/other.onnx",
      "error": "static check supports .tflite models in this release; cannot analyse other.onnx (ONNX static analysis is a follow-on)"
    }
  ]
}
```

Every backend result carries `"source": "static"` -- this slice ships
fidelity tier 1 only, a conservative estimator with no NPU toolchain
involved (round SRAM up, treat unknown/unsupported ops as
CPU-fallback, assume <=50% NPU utilisation for latency -- a false
"fits" is the worst failure this analyzer can make).  Tier 2
(manufacturer-precomputed bench data, `source: "precomputed"`) and
tier 3 (exact, on-demand toolchain compile) are follow-ons, as is ONNX
static analysis.  `budget_sram_kib` is `null` until a SoC's
`inference_arena_sram_kib` metadata field is authored -- every SoC's
value is currently a `0` placeholder, so `no-fit` can never fire from
an SRAM overflow yet, and the in-family cross-sell suggestion (move to
a larger-arena SoM) stays dormant until those budgets exist.

#### `tan model zoo` -- browse the curated model zoo

```bash
tan model zoo --sku E1M-AEN801           # entries marked for this SoM
tan model zoo --board path/to/board.yaml # SKU from the board's `som.sku`
tan model zoo --format json
```

Lists the curated model-zoo entries (`metadata/model_zoo/<id>.yaml`),
each flagged `runs_here` for the target SoM -- computed from the entry's
`validated_soms` against the SKU (`--sku`) or the board's `som.sku`
(`--board`).  Link, fetch, and layer only -- no model weights are
redistributed.  `--format` is `human` (default) or `json`.

#### `tan model add` -- pull a zoo entry into board.yaml

```bash
tan model add <zoo-id> --board path/to/board.yaml
tan model add <zoo-id> --board path/to/board.yaml --name my-detector
tan model add <zoo-id> --board path/to/board.yaml --models-dir models/
```

Fetches the zoo entry's source -- a URL verified against its recorded
sha256, or a bundled asset -- and appends a `{name, source}` item to the
board's `models:` list.  Non-destructive: a duplicate model `name` is an
error, never an overwrite.  `--name` overrides the derived entry name;
`--models-dir` sets where the fetched source lands.

#### `tan model prep` -- license-free INT8 quantize + accuracy report

```bash
tan model prep raw.onnx --calibration path/to/calib_dir
tan model prep raw.onnx --calibration path/to/calib_dir --out raw.int8.onnx
tan model prep raw.onnx --calibration path/to/calib_dir --per-channel
tan model prep raw.onnx --calibration path/to/calib_dir --format json
```

Quantizes an ONNX model to INT8 (QDQ static quantization) and reports
the **fp32-vs-int8 accuracy delta** -- turning "quantization is a dark
art" into a guided, measured flow. Entirely on free `onnxruntime`/`onnx`
(`onnxruntime.quantization` + `onnxruntime.InferenceSession`); no
vendor toolchain (Vela/dxcom/DRP-AI) is invoked here -- that compile
step is a separate, later stage.

`--calibration` is a directory of `*.npy` samples, each a single-sample
batch matching the model's input shape (dynamic/-1 dims wildcard). The
calibration set is validated *before* quantizing: fewer than
`--min-samples` (default 8) samples, or any sample whose shape doesn't
match the model input, is a clear error -- a bad calibration set is the
silent accuracy killer this guards against. `--per-channel` enables
per-channel weight quantization, which often recovers accuracy lost to
per-tensor quantization. `RAW` must be `.onnx` or `.tflite`; other
formats are a clear "not supported" error, never a wrong result.
Requires the `model-prep` extra (`pip install alp-sdk-cli[model-prep]`).

`.tflite` (the SDK's native format) is converted to ONNX via `tf2onnx`
before quantizing -- the intermediate ONNX is a temp file, cleaned up
after `prep` finishes (or fails). This needs the additional `model-convert`
extra (`pip install alp-sdk-cli[model-convert]`, which adds `tf2onnx` +
`tensorflow`) -- kept separate from `model-prep` because it's heavy
(pulls in tensorflow); a `.tflite` input without it errors with a clear
message naming the extra, never a raw import traceback. `.onnx` input
still only needs the lighter `model-prep` extra. PyTorch/Keras -> ONNX
conversion is a further follow-on.

After quantizing, both the fp32 and INT8 models run on every
calibration sample and the outputs are compared: top-1 agreement %,
mean cosine similarity, and max absolute error, rolled up into a
`verdict` of `good` or `degraded` (top-1 agreement below 95% by
default) plus concrete `guidance` when degraded (try `--per-channel`,
add more representative calibration data, keep sensitive ops in fp16).
A quantize or inference failure exits 1 -- it never emits a "prepped"
model that didn't actually quantize or that can't run.

`--format json` payload:

```json
{
  "raw": "raw.onnx",
  "quantized": "raw.int8.onnx",
  "accuracy": {
    "samples": 8,
    "top1_agreement_pct": 100.0,
    "mean_cosine": 0.9998,
    "max_abs_err": 0.010144,
    "verdict": "good",
    "guidance": null
  }
}
```

#### `tan model run` -- host reference run (functional + host latency + accuracy)

```bash
tan model run model.onnx                              # deterministic random input
tan model run model.onnx --input sample.npy            # a real input sample
tan model run model.onnx --input sample.npy --expected 3
tan model run model.onnx --runs 50 --format json
tan model run model.onnx --on-device                   # + a real on-target energy measurement
```

Loads the ONNX model into `onnxruntime` on the **CPU** and runs it: a
functional smoke test (does it load and produce an output) plus a
**host** latency number (`latency_ms`, the median of `--runs`, default
20, timed inferences after one warm-up run) and (with `--expected`) a
top-1 accuracy check against a known label. `MODEL` must be `.onnx` in
this release. Without `--input`, a deterministic random sample
(seeded, matching the model's first input shape) is used and the
payload marks `"random_input": true`.

**This is not the target SoM's performance.** `backend` is always
`"cpu-host"` and every payload -- human or JSON -- carries a `note`
stating the host-reference caveat explicitly. `peak_sram_kib` and
`power_mj` stay `null` here (the on-device arena high-water mark and
per-rail power reads are a separate, still-HW-gated surface).
`energy` stays `null` unless `--on-device` is given (see below). A
model that fails to load or run raises a clean error (exit 1), never
a raw traceback.

**`--on-device`** -- after the host reference run, also runs a REAL
on-target energy measurement: builds and RAM-runs an energy-sampling
app on an Alif Ensemble E8 (AEN801), reads back its console capture (a
`ENERGY-CFG`/`ENERGY-S`/`ENERGY-W`/`ENERGY-RESULT` protocol over the
on-board INA236 shunt monitor), and idle-subtracts active vs. idle
windows into mJ/inference. This needs a **held labgrid reservation**
for the target bench place and real hardware -- it never acquires or
releases that reservation itself (that is the human/orchestrator's
responsibility). On any failure (no reservation held, build/flash
failure, malformed capture) it **errors out (exit 1) instead of
reporting a null or fabricated energy figure** -- a silent `null`
would read as "measured nothing" when the truth is "the bench run
failed". On success, the JSON payload gains a top-level `on_device`
key with cross-check diagnostics: `cycles_per_s_used`,
`cycles_per_s_dt`, `cycles_per_s_measured` (the DT-constant-vs-
kernel-uptime reconciliation), `device_value_mj_per_inference` (the
on-target app's own computed figure, `null` when the capture carried
no `ENERGY-RESULT` line), `host_vs_device_ratio`, `npu_dispatched`,
`windows`, and `werr_lines`/`warn_lines` (the raw `ENERGY-WERR`/
`ENERGY-WARN` diagnostic lines seen, so a degraded run -- I2C errors, a
timed-out window, a too-long window span -- stays visible instead of
looking identical to a clean one). The human-readable form says the
device cross-check is unavailable rather than printing a formatted
`None` when there is no on-target result to compare against.

`--format json` payload:

```json
{
  "model": "model.onnx",
  "backend": "cpu-host",
  "latency_ms": 3.214,
  "output_argmax": 5,
  "peak_sram_kib": null,
  "power_mj": null,
  "energy": null,
  "runs": 20,
  "random_input": false,
  "note": "host reference run (backend=cpu-host): functional + host latency, NOT the target SoM. On-device latency/SRAM/power is the HW-gated follow-on.",
  "accuracy": { "expected": 3, "match": false }
}
```

`accuracy` is present only when `--expected` is given. `energy` is
`null` on the host reference run shown above (no `--on-device`); with
`--on-device` and a successful bench run it is a structured object:
`{"source": "measured", "scope": "carrier-rail-delta",
"value_mj_per_inference", "rails", "n_inferences", "window_ms",
"sample_count", "spread_mj"}`. `source` and `scope` are fixed,
validated labels -- `energy` is only ever `null` or that honest
board-level-delta shape, never a fabricated or silicon-scoped number,
and a failed `--on-device` run exits non-zero rather than emitting a
payload with `energy: null` (see above).

#### `tan model ab` -- A/B two models on the host (latency + size)

```bash
tan model ab a.onnx b.onnx                             # deterministic random input
tan model ab a.onnx b.onnx --input sample.npy --runs 50
tan model ab a.onnx b.onnx --format json
```

Runs both `MODEL_A` and `MODEL_B` on the host (same `onnxruntime` CPU
session machinery as `tan model run`) with the **same** input sample,
then compares them: which is faster, the latency ratio (`b/a`), and
the file-size delta. Both models must be `.onnx`; the shared input
defaults to a deterministic random sample sized from `MODEL_A`'s input
(or `--input` if given). The comparison itself is pure/deterministic
given the two measured results -- no additional timing happens inside
it. Same host-reference caveat as `model run`: `backend` is
`"cpu-host"` for both sides and the payload carries the same `note`.

`--format json` payload:

```json
{
  "a": { "model": "a.onnx", "backend": "cpu-host", "latency_ms": 3.2, "output_argmax": 5, "runs": 20, "peak_sram_kib": null, "power_mj": null, "energy": null },
  "b": { "model": "b.onnx", "backend": "cpu-host", "latency_ms": 1.8, "output_argmax": 5, "runs": 20, "peak_sram_kib": null, "power_mj": null, "energy": null },
  "comparison": { "faster": "b", "latency_ratio": 0.5625, "a_latency_ms": 3.2, "b_latency_ms": 1.8, "size_delta_bytes": -40960 },
  "note": "host reference run (backend=cpu-host): functional + host latency, NOT the target SoM. On-device latency/SRAM/power is the HW-gated follow-on."
}
```

Each side's `energy` is `null` on the host reference run shown above
(same structured-object-or-null shape as `model run`, see above).
`comparison.energy_delta_mj_per_inference` (`b.energy -
a.energy`, mJ/inference, rounded to 4 decimals) appears in
`comparison` only when **both** `a` and `b` carry a real (non-null)
`energy` measurement -- it is omitted entirely, not `null`, when
either side is a host-only run (as above).

**The self-improving loop.** `alp_model.measure.estimate_vs_measured(est_latency_ms,
measured_latency_ms)` (not yet wired to a CLI flag) is the intended
bridge from `tan model check`'s static tier-1 `est_latency_ms` estimate
to a `tan model run` measured `latency_ms`: it returns
`{"est_latency_ms", "measured_latency_ms", "ratio"}` (`ratio =
measured/est`, or `null` if there was no estimate). Repeated across
models, that ratio is the feedback signal that (later) calibrates the
tier-1 estimator against reality instead of staying a fixed guess.

### `tan doctor` -- debug-readiness preflight {#tan-doctor}

`tan doctor` has two distinct modes behind one subcommand -- neither
takes `--strict`, and both use `[+]` (pass) / `[!]` (warn) / `[x]`
(fail) markers, not `[PASS]`/`[WARN]`/`[FAIL]`.  `alp-sdk`'s own
separate Python preflight, `python -m alp_cli doctor`, is a third,
unrelated command with its own check list -- see the note at the end
of this section.

```bash
tan doctor                     # debug readiness (below)
tan doctor --format json       # machine-readable
```

Diagnoses whether a target/server combination is ready for a debug
session -- checks `workspaceRoot`, `sdkRoot`, `boardYaml`, a `python`
interpreter presence probe (no `.python-version` pin comparison),
`codeLLDBExtension`, `lldb`, and `sdkProvenance`.  See `tan doctor
--help` for the full flag reference (including `--fix`, `--target`,
and `--all`) -- not restated here to avoid a second copy drifting from
the CLI.  Example:

```
  tan doctor  native-host · none

  [+]  workspaceRoot       /work/alp-sdk
  [+]  sdkRoot             /work/alp-sdk
  [x]  boardYaml           /work/alp-sdk/board.yaml
  [+]  python              Interpreter probe: python3
  [+]  codeLLDBExtension   vadimcn.vscode-lldb is installed.
  [!]  lldb                No local LLDB executable was found on PATH.
  [+]  sdkProvenance       alp-sdk 0.13.0 @ 08230793

  5 passed · 1 warning · 1 failed
```

### `tan doctor --build` -- build-readiness preflight

```bash
tan doctor --build                       # human-readable report
tan doctor --build --format json         # machine-readable
```

This is the check to run first whenever a build machine misbehaves:
checks `sdk`, `boardYaml`, `workspace`, `westResolved`,
`zephyrVersion` (the Zephyr pin, read live from `west.yml`), `west`,
`cmake`, `ninja`, `zephyrSdk`, `bitbake`, `bmaptool`,
`vendorToolchain`, and `sdkProvenance`.  Exit 0 = ready to build;
every `[!]`/`[x]` comes with a `fix` remediation hint.  Example:

```
  tan doctor --build  zephyr · yocto · baremetal

  [+]  sdk               alp-sdk at /work/alp-sdk
  [x]  boardYaml         board.yaml not found — run `tan init` or pass `--board-yaml <path>`
  [+]  workspace         Zephyr workspace at /work
  [+]  westResolved      west resolved
  [+]  zephyrVersion     Zephyr v4.4 matches the SDK pin
  [+]  west              west is available.
  [+]  cmake             cmake is available.
  [!]  ninja             ninja not found on PATH — needed for Zephyr builds.
  [!]  zephyrSdk         Zephyr SDK toolchain not detected (ZEPHYR_SDK_INSTALL_DIR unset).
  [!]  bitbake           bitbake not found on PATH — needed for Yocto builds.
  [!]  bmaptool          bmaptool not found; Yocto .wic flash falls back to dd (slower).
  [!]  vendorToolchain   Baremetal needs a vendor toolchain (Alif/Renesas/NXP), per SoC family.
  [+]  sdkProvenance     alp-sdk 0.13.0 @ 08230793

  7 passed · 5 warnings · 1 failed
```

Note: `python -m alp_cli doctor` (alp-sdk's own separate Python
preflight -- `pyproject.toml` deliberately ships no `alp`
console-script) is a different tool again, with the fuller check list
(Python / west / CMake / Ninja / dtc / gperf / imgtool / host
compiler / J-Link presence, the workspace venv, `ZEPHYR_BASE`,
Windows-specific traps) and its own `--strict` / `--json` flags and
`[PASS]`/`[WARN]`/`[FAIL]` markers.  It predates the `tan` CLI
migration and is not what either `tan doctor` invocation above runs.

### `tan monitor` -- serial console

<!-- cross-platform-lint:ignore -->
```bash
tan monitor --port COM7                       # Windows
tan monitor --port /dev/ttyUSB0               # Linux
tan monitor --port /dev/cu.usbserial-1420     # macOS
tan monitor                                    # lists available ports if none given
```
<!-- cross-platform-lint:resume -->

Opens pyserial's miniterm (Ctrl+] to quit).  Baud defaults to 115200
(`--baud` to override).  When no port is given or the requested one
doesn't exist, it lists every serial port on the host and exits
non-zero instead of hanging on a wrong device.

### `tan explain` -- decode a diagnostic code

```bash
tan explain ALP-B001
```

Prints the cause, fix, and doc link for any `ALP-Bxxx` validator
diagnostic (the codes `tan validate` emits).

### `tan faultdecode` -- decode a Cortex-M fault dump

```bash
tan faultdecode fault.txt
```

Decodes an ARMv8-M (M33/M55) fault-register dump into a
human-readable cause chain.

## One-liner build + flash examples (`tan`)

```bash
# AEN (E1M-AEN801 M55): build every slice + flash over SWD, one line.
cd examples/peripheral-io/gpio-button-led && tan build && tan flash

# V2N helper MCU only (GD32 bridge), preview first:
tan flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge --dry-run
tan flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge

# One slice of a heterogeneous system:
tan flash examples/multicore/rpmsg-v2n --core m33_sm
```

These are `tan` invocations -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)
for the full flag reference.  alp-sdk itself never builds or flashes --
it emits build plans only.

## Environment

| Variable | Effect |
|---|---|
| `ALP_SDK_ROOT` | Explicit path to the alp-sdk checkout; otherwise the CLI locates the repo it was installed (editable) from |
| `ZEPHYR_BASE` | The Zephyr tree checked by `tan doctor --build` |

`west alp-emit` exports `ALP_SDK_ROOT` and puts `<sdk>/scripts` on
`PYTHONPATH` for its sub-process, so a west-invoked orchestrator run
behaves identically to running `alp_orchestrate` directly.  `tan
generate` does its own SDK-root resolution and invokes
`<sdk_root>/scripts/alp_project.py` by its full path with plain
`--input`/`--emit`/`--output` arguments instead -- no env wiring, since
the script is addressed directly rather than imported as a module.
`tan` separately reads the plan's own `env` / `envAppendPath` entries
(sourced from the SDK's `--emit build-plan`) to set up
`EXTRA_ZEPHYR_MODULES` and `PYTHONPATH` for the slices it builds --
see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli).

## See also

- [getting-started.md](getting-started.md) -- install + first build.
- [heterogeneous-builds.md](heterogeneous-builds.md) -- the
  orchestrated multi-core pipeline `alp_orchestrate` plans and `tan
  build` fronts.
- [board-config-schema.md](board-config-schema.md) -- the `board.yaml`
  field reference `tan validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `tan doctor --build`
  isn't enough.
- [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) -- the
  standalone executor's own docs (`tan build` / `flash` / `size` /
  `image` / `clean` / `renode`).
