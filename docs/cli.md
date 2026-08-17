# The `tan` CLI

`tan` is the Alp SDK's user-facing command line (ADR
[0020](adr/0020-sdk-owns-build-execution.md)): it scaffolds and validates
projects, renders configuration, diagnoses the host, builds and flashes every
core, packages images and models, opens a serial console, and provides the
inspection/debugging commands used by Alp IDE.

The current implementation is Python and is independently versioned in
[`alplabai/tan-cli`](https://github.com/alplabai/tan-cli). As of
[v0.5.0](https://github.com/alplabai/tan-cli/releases/tag/v0.5.0) (current
release: [v0.5.1](https://github.com/alplabai/tan-cli/releases/tag/v0.5.1)),
the published release installs the real Python `tan` directly -- it no
longer resolves the frozen Rust v0.4.1 release. alp-sdk `dev` tracks
`tan-cli/dev` instead, to stay ahead of the last tagged release; install the
Python development line into an isolated Python 3.12+ environment:

```bash
git clone --branch dev https://github.com/alplabai/tan-cli
python3 -m venv tan-cli/.venv
tan-cli/.venv/bin/python -m pip install ./tan-cli/python
export PATH="$PWD/tan-cli/.venv/bin:$PATH"
```

```powershell
# Windows PowerShell
git clone --branch dev https://github.com/alplabai/tan-cli
py -3.12 -m venv tan-cli/.venv
& .\tan-cli\.venv\Scripts\python.exe -m pip install .\tan-cli\python
$env:Path = "$(Resolve-Path .\tan-cli\.venv\Scripts);$env:Path"
```

From tan v0.5.0, release assets are PyInstaller `--onedir` archives (`.tar.gz`
on Unix, `.zip` on Windows). They bundle their Python runtime, so release users
need neither Python nor Rust and can use the ordinary installers:

```bash
curl -fsSL https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/alplabai/tan-cli/main/install.ps1 | iex
```

For manual asset selection, checksum/provenance verification, system-wide
installation, and the measured Linux glibc floor, use tan-cli's own
[installation reference](https://github.com/alplabai/tan-cli#install). Do not
copy a raw-binary URL from the pre-v0.5 layout: the Python release is an archive
containing `tan` plus its `_internal/` runtime. Alp's `tan` is not distributed
on PyPI, and the bare name `tan` there belongs to an unrelated project (the
PyPI JSON API returns `200` for it: `tan` v23.7.0, "The compromising code
formatter") -- `pip install tan` does not get you this tool. `alp-tan` is not
registered there either (`404` for it, not a reservation placeholder).
`cargo install` installs the stale v0.4.1 program and is not a supported way
to get current Tan.

## Where planning and commands live

Python Tan contains the shipping planner and executor. Its relocated planner
reads `board.yaml` plus metadata and schemas from the selected alp-sdk checkout,
produces a build plan in process, and the executor materialises and runs that
plan. The build-plan JSON remains a deliberate planner/executor seam, but it is
now internal to Tan's normal build path.

alp-sdk keeps the original planner and emitters as the canonical reference and
parity producer while the port settles. Direct `python -m alp_orchestrate` /
`python -m alp_cli emit` and `west alp-emit` calls are maintainer/inspection
surfaces; normal `tan build` does not import or spawn
`scripts/alp_orchestrate/`. `tan generate` renders its supported targets from
the relocated planner in process and uses the SDK subprocess only as an explicit
or reported compatibility fallback.

Most of Tan's 32 verbs are native Python implementations, including `doctor`,
`init`, `validate`, `generate`, `model`, `new-som`, `faultdecode`, `monitor`,
`run`, and the build/flash/image/simulation surface. Only `migrate`, `lock`, and
`quality` still forward to the corresponding `west alp-*` maintenance command.
The seven former stubs (`scaffold`, `completion`, `diff`, `pinmux`, `inspect`,
`trace`, `support-bundle`) are implemented too. The SDK's `alp_cli` package is
still installed in the workspace venv for direct SDK maintenance, parity, and
the surviving west commands, but it is no longer Tan's command backend.

No `alp` console script is installed (`pyproject.toml` registers only
`alp-mcp`). Maintainers can run the SDK reference CLI straight from a checkout:

```bash
PYTHONPATH=scripts python3 -m alp_cli --help
```

### `tan` vs `python -m alp_cli` -- five verb names collide, contracts differ

`scripts/alp_cli` (this repo's internal reference/parity package) and `tan`
register several identically-named verbs that do **different jobs**. Do not
read `python -m alp_cli <verb> --help` as documentation for `tan <verb>`, or
vice versa -- five names collide with an incompatible contract
(alp-sdk#1193):

| Verb | `python -m alp_cli` contract (measured) | `tan` contract |
|---|---|---|
| `generate` | `generate TEMPLATE_ID DEST [--dry-run] [--force] [--param name=value]` -- materialises a **catalog template** into a directory (`scripts/alp_cli/generate.py:67-90`) | `tan generate --target <mode>` (also `--all`, `--core`, `--output`, `--force`) -- emits a **board-derived config artefact**; no positional `TEMPLATE_ID`/`DEST` (see [`#tan-generate`](#tan-generate) below) |
| `init` | `init NAME [--som] [--preset] [--peripherals]` -- positional project name (`scripts/alp_cli/init.py:76-90`) | `tan init` -- **options only**, no positional `NAME` (see "`tan init`" below) |
| `doctor` | `doctor [--json] [--strict] [--no-color]` (`scripts/alp_cli/doctor.py:789-793`) | `tan doctor` (also `--format json`, `--build`) -- `--json` is spelled `--format json`; `--strict` has no `tan` equivalent (see "`tan doctor`" below) |
| `explain` | `explain CODE [--json] [--no-color]` -- looks up an `ALP_ERR_*`/`ALP-Bxxx` **diagnostic code** (`scripts/alp_cli/explain.py:68-74`) | `tan explain [--template] [--target]` -- describes a **project/module template or generation target**; also takes an optional positional `[TEMPLATE]` (`python/tan/commands/explain_cmd.py`), but never a diagnostic `CODE` lookup (see "`tan explain`" below) |
| `run` | `run [--board] [--flash]` -- one direct `west build` + optional flash, single image (`scripts/alp_cli/run.py:76-79`) | `tan run` (also `--flash`, `--core`) -- a **distinct command**, not an alias for `tan build`/`tan flash`: builds the full multi-slice plan, then for a `native_sim` target executes the produced binary, or for a hardware target (with `--flash`) flashes it (`python/tan/commands/run_cmd.py`) |

Four more `alp_cli` verbs (`model`, `monitor`, `new-som`, `faultdecode`)
share a name with a `tan` verb too but were not found to diverge in shape on
inspection -- they are not re-verified field-by-field here, so do not treat
"same option list" as a proven-identical envelope either. `emit` is the one
`alp_cli` verb with no `tan` namesake (`tan` reaches the same ground via
`tan generate --target <mode>`). `alp_cli` registers 11 verbs today
(`scripts/alp_cli/main.py:27-37`: `doctor`, `emit`, `explain`, `faultdecode`,
`generate`, `init`, `model`, `monitor`, `new-som`, `run`, `validate`) --
`build` is not one of them; ADR 0020 retired the SDK-side fan-out executor
and no `build.py` remains in `scripts/alp_cli/` to collide with `tan build`.

None of the five is planned to become a console-script front door under its
current contract -- `scripts/alp_cli/__main__.py`'s own docstring names this
issue and refuses the `alp` binary framing. That is the only part of
alp-sdk#1193 this table closes. The rest of #1193's required work is still
open: a per-verb port/rename/delete disposition for every `alp_cli` verb (not
just these five), removing the `alp`-prefixed front-door language that
`__main__.py` disclaims but the other modules still carry (e.g.
`main.py`'s `prog_name="alp"`, `init.py`'s scaffolded "Generated by `alp
init`." README text, and the `alp <verb>:` message prefixes across
`doctor.py`/`emit.py`/`generate.py`/`init.py`/`monitor.py`/`new_som.py`/
`run.py`), and parity tests. This table exists so no one assumes name parity
as documentation in the meantime; the disposition and cleanup remain a
separate, not-yet-started piece of work.

A sixth verb, `validate`, also diverges on inspection though it is not one
of the five alp-sdk#1193 names: `python -m alp_cli validate` takes an
**optional** positional `PATH` (default `board.yaml`, so a bare invocation
works) and `--format human|json|sarif` (`scripts/alp_cli/validate.py:16-20`),
while `tan validate` is options-only (`--board-yaml`, `--offline`) with
`--format text|json|diagnostic-v1|sarif` (see "`tan validate`" below) --
different vocabulary, different flag names, and `alp_cli` accepts the
positional form `tan` does not. Flagged here for the same disposition work;
not added to alp-sdk#1193's own five-name scope.

## `tan` vs `west alp-*` -- which one do I use?

Two front doors, two different jobs -- pick by what you're doing:

| You are... | Use |
|---|---|
| Scaffolding a project, validating `board.yaml`, compiling a model, checking your host, opening a serial console, explaining a template/generation target, decoding a fault, or running a native_sim/single-board loop | `tan init` / `tan new-som` / `tan validate` / `tan model` / `tan doctor` / `tan monitor` / `tan explain` / `tan faultdecode` / `tan run` |
| Inspecting a board-derived config artefact without building | `tan generate --target <mode>`; see the target table below |
| Inspecting an orchestrator-owned artefact without building (system manifest, build plan, Kconfig menu, IPC contract header, DTS reservations/partitions, storage mounts, TF-M sysbuild overlay) | `west alp-emit <mode>` from a west workspace |
| Building, flashing, sizing, bundling, or cleaning a project | `tan build` / `tan flash` / `tan size` / `tan image` / `tan clean` -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) |
| Scripting the surviving west-centric maintenance commands | `tan migrate` / `tan lock` / `tan quality` (forwarders), or their direct `west alp-*` forms; `west alp-emit` remains the generated-artefact subset |

Rules of thumb:

* `tan build` plans in process, materialises shared/per-slice artefacts, then
  drives `west` / `bitbake` / `cmake`. `--native` is the explicit spelling of
  the default. `--plan-from FILE` reads a reviewed plan and shows it; add
  `--materialise` to write it or `--execute` to write and run it. The legacy
  `--plan` flag is accepted but deferred in the Python transition, so do not
  teach it as the way to inspect a newly generated plan.
* `tan generate` and `west alp-emit` cover different artefact sets. Tan reaches
  nine default board-derived targets plus three explicit-only targets; west
  reaches the eight orchestrator/reference targets. The table below is the
  authoritative split.
* `tan sdk list` and `tan sdk current` work. `tan sdk install` and `tan sdk
  switch` currently refuse with `sdk.not-ported`; use `--sdk-root`, a project
  `.alp/sdk-path` written by `tan init --sdk-root`, or a sibling `alp-sdk/`
  checkout.

## Verb reference

### `tan init` -- scaffold a new project

```bash
tan init --name my-app --som E1M-AEN801 --template zephyr-app --sdk-root <path-to-alp-sdk>
tan init --preview           # preview non-interactive defaults without writing
```

`tan init` takes options only -- there is no positional `NAME`
argument.  Scaffolds a new project directory (named by `--name`),
rewrites `board.yaml` for the chosen SoM SKU + template, and resolves
the alp-sdk checkout the project should build against via
`--sdk-root`. Interactive init prompts are not yet ported; omitted values use
the command's documented defaults.

| Option | Meaning |
|---|---|
| `--name` | New project directory name |
| `--som` | SoM SKU, e.g. `E1M-AEN801` |
| `--template` | Project template id, e.g. `zephyr-app` (the default), `sensor-starter` |
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
family defers this to the checklist).  "Exists" is decided on the file,
not on whether it could be read: a family table that is present but
unusable -- empty, truncated above its `hw_revisions:` block, a bare
scalar, or not valid YAML -- is refused by name rather than treated as
absent, so a damaged table cannot silently skip the `--default-hw-rev`
cross-check (alplabai/tan-cli#563).  The generated
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

### `tan build` / `flash` / `size` / `image` / `clean` -- build execution

Before ADR [0020](adr/0020-sdk-owns-build-execution.md) these were
`alp build` / `alp flash` / `alp size` / `alp image` / `alp clean` /
`alp renode` (and their `west alp-*` twins), fronted by the now-removed
`alp` console script. ADR-0020 Phase 4
retired the SDK-side fan-out executor (`Orchestrator.fan_out()`,
`_dispatch_slice()`, and the
`west alp-{build,image,flash,clean,size,renode}` extensions).  The
multi-slice build/flash/size/image/clean surface moved to the
standalone, public **`tan` CLI**.  The sixth slot in those historical
lists, `renode`, has no successor: it was retired outright rather than
moved -- see [ADR 0022](adr/0022-python-executor-renode-retirement.md),
Amendment 2, and "Verifying without a board" in
[heterogeneous-builds.md](heterogeneous-builds.md) for what to do
instead.

```bash
tan build                          # materialise the plan + run every slice's build command (default)
tan build --project my-app         # same, for a project outside the cwd
tan build --plan-from plan.json    # inspect a reviewed plan file; no writes or build
tan build --plan-from plan.json --materialise  # write it, but do not run it
tan build --plan-from plan.json --execute      # write and execute it
tan flash                          # program every slice + helper MCU
tan size --fail-over-budget        # footprint vs the SoM memory budget
tan image                          # assemble a flashable bundle
tan clean                          # remove build outputs
```

Python Tan's relocated planner produces the machine-readable, write-free build
recipe in process -- one entry per non-`off` core, with the resolved app source
dir, exact tool command, and environment. The executor materialises the plan and
writes its `system-manifest.yaml`. The plan is hermetic: under
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
for the migration history and current amendment.

alp-sdk's original reference planner remains directly inspectable without
`tan`; these commands are the parity/oracle view, not what normal `tan build`
spawns:

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
tan generate --target hw-info-h           # build-time hardware-identity header
tan generate --target west-libraries      # board-scoped west manifest fragment
tan generate --all                        # all nine default targets
tan generate --target ipc-contract-h      # explicit-only cross-core header
tan generate --target zephyr-board --core m55_hp  # explicit-only board tree
```

`tan generate` writes files; stdout carries only its text/JSON result envelope.
Each target has a conventional destination. `--output <path>` overrides it for
a single target, and `--force` permits replacing a generated native_sim overlay.

| Target | Written to |
|---|---|
| `zephyr-conf` | `build/generated/alp.conf` |
| `dts-overlay` | `build/generated/alp.overlay` |
| `native-sim-overlay` | `boards/native_sim_native_64.overlay` (the app's own source tree, not `build/`) |
| `cmake-args` | `build/generated/alp-cmake-args.txt` |
| `yocto-conf` | `build/generated/alp-yocto.conf` |
| `carrier-netlist` | `build/generated/carrier-netlist.json` |
| `west-libraries` | `build/generated/alp-west-libs.yml` |
| `hw-info-h` | `build/generated/alp_hw_info_build.h` |
| `os-topology` | `build/generated/os-topology.json` |
| `composed-route-table` (explicit only) | `build/generated/composed-route-table.json` |
| `ipc-contract-h` (explicit only) | `build/generated/alp/system_ipc.h` |
| `zephyr-board` (explicit only; requires `--core`) | generated board directory selected from SKU + core |

The Python port closed the old six-target gap. Nine targets participate in a
bare `tan generate` / `--all`; `composed-route-table`, `ipc-contract-h`, and
`zephyr-board` are explicit-only because they are respectively a maintainer
debug view, project-dependent on an `ipc:` declaration, and core-scoped.

The full SDK emit split is:

| Mode | Artefact | Owned by | Reachable via |
|---|---|---|---|
| `zephyr-conf`, `cmake-args`, `yocto-conf`, `dts-overlay`, `native-sim-overlay`, `carrier-netlist`, `west-libraries`, `hw-info-h`, `os-topology` | Board/per-core configuration | SDK metadata + relocated Tan renderers | `tan generate` (default/`--all`) |
| `composed-route-table` | Route-composition debug JSON | SDK metadata + relocated Tan renderer | `tan generate --target composed-route-table` |
| `ipc-contract-h` | Cross-core IPC contract header | orchestrator | `tan generate --target ipc-contract-h`, `west alp-emit ipc-contract-h` |
| `zephyr-board` | Per-core Zephyr board tree | SDK metadata + relocated Tan renderer | `tan generate --target zephyr-board --core <id>` |
| `system-manifest`, `dts-reservations`, `dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`, `build-plan` | Cross-core/system artefacts | orchestrator/reference planner | `west alp-emit <mode>` or direct SDK Python |
| `kconfig` | Workspace-resolved Kconfig menu | orchestrator/reference planner + Zephyr | `tan kconfig`, `west alp-emit kconfig` |
| `scaffold` | New-project `{path, contents}[]` envelope | SDK template catalog, vendored into Tan | `tan init` / `tan scaffold`; direct SDK emit for parity |

Tan's in-process renderers consume SDK metadata; hardware truth has not moved
out of alp-sdk. `TAN_GENERATE_EXECUTOR=subprocess` explicitly selects the
reference `scripts/alp_project.py` engine for parity/debugging. In automatic
mode, an unavailable in-process engine falls back with a visible
`generate.in-process-unavailable` warning and reports `data.engine=subprocess`;
the fallback is never silent.

| Option | Meaning |
|---|---|
| `--target` | Generate one named target |
| `--all` | Generate all nine default targets (same set as omitting `--target`) |
| `--core` | Required for `zephyr-board`; optional for the documented per-core targets |
| `--output` | Override the conventional destination for one target |
| `--force` | Permit overwriting an existing native_sim overlay |
| `--project`, `--board-yaml`, `--sdk-root`, `--format`, `--verbose`, `--quiet` | See `tan generate --help` for the live reference |

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
# also: tan kconfig --core m55_he         (the customer-facing path)
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

Python Tan's top-level `tan kconfig --core <id>` runs the relocated renderer in
process and wraps the result in its versioned envelope. The direct SDK and west
forms remain the parity/reference paths.

### `tan validate` -- check a board.yaml

```bash
tan validate --offline                              # ./board.yaml, human output
tan validate --offline --board-yaml path/to/board.yaml
tan validate --offline --format diagnostic-v1 --board-yaml path/to/board.yaml
tan validate --offline --format json --board-yaml path/to/board.yaml
tan validate --offline --format sarif --board-yaml path/to/board.yaml
```

`--offline` runs Tan's bundled structural checks without resolving an SDK.
Without `--offline`, Python Tan currently reports that the SDK-backed rich
validator subprocess is not yet ported (exit 2); use the direct SDK reference
command below when you need schema, preset, capability, and orchestrator
consistency checks during this transition.

`--format` selects the rendering:

- `text` (default) -- human-readable Tan output.
- `json` -- Tan's versioned command envelope.
- `diagnostic-v1` -- the SDK diagnostic machine document
  (`metadata/schemas/diagnostic-v1.schema.json`): `schemaVersion` is
  a version/capability handshake a consumer must check before
  parsing further, and every range is **zero-based** (LSP
  `Position`/`Range` convention).
- `sarif` -- a SARIF 2.1.0 log (`runs[].results[]`); SARIF regions
  are **one-based** by spec, the opposite of the `json` format's
  ranges -- the two exporters intentionally do not share range
  values.

`json`/`diagnostic-v1`/`sarif` print only the structured document to stdout, no
interleaved human prose.

The full SDK reference validator remains:

```bash
python3 scripts/validate_board_yaml.py --board-yaml path/to/board.yaml
```

It runs the rich validator plus the reference orchestrator consistency pass.

### `tan model` -- compile + package AI models

```bash
tan model build                          # compile board.yaml `models:` entries
tan model build --board path/to/board.yaml --out build/models
```

Compiles every `models:` entry declared in `board.yaml` into a
`.alpmodel` package via the SoM-appropriate backend (Vela for
Ethos-U, DRP-AI for RZ/V2N, ...).  See the model-pipeline docs under
`docs/tutorials/` for the end-to-end inference flow.

### `tan doctor` -- build and flash readiness {#tan-doctor}

```bash
tan doctor                     # human-readable report
tan doctor --format json       # machine-readable
tan doctor --build             # compatibility spelling; same checks
```

Python Tan runs one unconditional build/flash-oriented checklist. It verifies
the SDK and `board.yaml`, the resolved west/Zephyr workspace and version pin,
the effective Python floor, host prerequisites, Zephyr SDK availability, and
programming dependencies such as J-Link and Alif SETOOLS. Every failed or
warning check includes a remediation hint; an unhealthy host exits 4.

`--build` is retained for existing callers but no longer changes the checklist.
In an interactive text-mode run with a TTY, `--fix` may run the manifest's
install commands. An install command that needs no elevation (macOS `brew`,
Windows `winget`) runs for any caller. It never spawns the `sudo` program: a
`sudo`-prefixed command (Linux `apt`/`dnf`) is refused with
`doctor.fix-needs-sudo` and printed to run by hand for a non-root caller, and
has its literal `sudo ` prefix stripped and the rest executed for a caller
who is already root. Without a TTY on both stdin and stderr -- and under
`--ci`, `--non-interactive`, or JSON mode -- repairs are disabled and the
commands are left as printed guidance.

`PYTHONPATH=scripts python3 -m alp_cli doctor` is alp-sdk's separate reference
preflight. It has its own checks, `--strict`/`--json` flags, and
`[PASS]`/`[WARN]`/`[FAIL]` markers. It is maintained for SDK development and
parity checks; it is not the implementation behind `tan doctor`.

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

### `tan explain` -- explain a project/module template or generation target

```bash
tan explain                          # list every topic (templates + targets)
tan explain --template sensor-driver # explain one project/module template
tan explain --target dts-overlay     # explain one generation target
```

`tan explain` has no positional code-lookup argument -- an `ALP-Bxxx`
validator diagnostic (the codes `tan validate` emits) is decoded by its
own `see: docs/diagnostics/ALP-Bxxx.md` hint line in `tan validate`'s
human-mode output, not by `tan explain`.

### `tan faultdecode` -- decode a Cortex-M fault dump

```bash
tan faultdecode --file fault.txt
```

Decodes an ARMv8-M (M33/M55) fault-register dump into a
human-readable cause chain.

## One-liner build + flash examples (`tan`)

```bash
# AEN (E1M-AEN801 M55): build every slice + flash over SWD, one line.
cd examples/peripheral-io/gpio-button-led && tan build && tan flash

# V2N helper MCU only (GD32 bridge), preview first:
tan flash examples/v2n/v2n-gd32-bridge-functional --helper gd32_bridge --dry-run
tan flash examples/v2n/v2n-gd32-bridge-functional --helper gd32_bridge

# One slice of a heterogeneous system:
tan flash examples/multicore/rpmsg-v2n --core m33_sm
```

These are `tan` invocations -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)
for the full flag reference. Python Tan owns the normal planner and executor;
alp-sdk retains metadata, schemas, and reference emitters used for parity.

## Environment

| Variable | Effect |
|---|---|
| `ALP_SDK_ROOT` | SDK fallback after `--sdk-root`, a project `.alp/sdk-path`, and a sibling checkout |
| `ZEPHYR_BASE` | Zephyr fallback when Tan cannot resolve the active west workspace |
| `TAN_GENERATE_EXECUTOR` | Set to `subprocess` only to force the SDK-reference `tan generate` fallback |

`west alp-emit` exports `ALP_SDK_ROOT` and puts `<sdk>/scripts` on
`PYTHONPATH` before invoking the SDK reference implementation. Normal
`tan generate` uses Tan's relocated in-process renderer; its visible transition
fallback invokes `<sdk_root>/scripts/alp_project.py` by full path.
`tan` reads the plan's own `env` / `envAppendPath` entries to set up
`EXTRA_ZEPHYR_MODULES` and `PYTHONPATH` for the slices it builds --
see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli).

## See also

- [getting-started.md](getting-started.md) -- install + first build.
- [heterogeneous-builds.md](heterogeneous-builds.md) -- the
  orchestrated multi-core pipeline Python Tan plans and executes.
- [board-config-schema.md](board-config-schema.md) -- the `board.yaml`
  field reference `tan validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `tan doctor`
  isn't enough.
- [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) -- the
  standalone executor's own docs (`tan build` / `flash` / `size` /
  `image` / `clean`).
