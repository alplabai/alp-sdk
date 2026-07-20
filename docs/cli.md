# The `alp` CLI

`alp` is the Alp SDK's command-line front door for scaffolding a
project, inspecting the generated configuration, validating
`board.yaml`, compiling AI models, sanity-checking the host
environment, opening a serial console, decoding a diagnostic code or a
fault dump, and scaffolding the metadata for porting a new SoM.

The plan-based, multi-slice build / flash / size / image / clean /
Renode surface is **not** `alp`'s job.  Per ADR
[0020](adr/0020-sdk-owns-build-execution.md), alp-sdk is
**plans-only** for that surface -- it lives in the standalone, public
**`tan` CLI** ([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)),
the SDK's primary build executor: it consumes
`alp_orchestrate --emit build-plan` / `--emit system-manifest` and
runs `west` / `bitbake` / `cmake` per slice.  Install `tan` separately
(needs a Rust toolchain / rustup): `cargo install --git
https://github.com/alplabai/tan-cli --bin tan`; see its own repo for
`tan build` / `tan flash` / `tan size` / `tan image` / `tan clean` /
`tan renode`.  `alp run` is the one exception: a retained
single-image escape hatch (shells `west build`, and `west flash` with
`--flash`) for a quick native_sim or single-board loop without a
`tan` install -- see the verb reference below.

`alp` is installed automatically by the bootstrap scripts
(`scripts/bootstrap.sh` on Linux/macOS/WSL2, `scripts/bootstrap.ps1`
on native Windows) as an editable install into the workspace venv, so
a `git pull` in the alp-sdk checkout updates the CLI in place.
Without an install it also runs straight from a checkout:

```bash
# From the alp-sdk repo root (or with <sdk>/scripts on PYTHONPATH):
PYTHONPATH=scripts python3 -m alp_cli --help
```

Every verb is a **thin wrapper**: the actual logic lives in the same
scripts the surviving `west alp-*` extension commands drive
(`scripts/alp_orchestrate/`, `scripts/alp_project.py`,
`scripts/validate_board_yaml.py`).  The CLI adds discoverability and
sane defaults; it never forks the behaviour.

## `alp` vs `tan` vs `west alp-*` -- which one do I use?

Three front doors, three different jobs -- pick by what you're doing:

| You are... | Use |
|---|---|
| Scaffolding a project, validating `board.yaml`, compiling a model, checking your host, opening a serial console, decoding a diagnostic/fault | `alp init` / `alp new-som` / `alp validate` / `alp model` / `alp doctor` / `alp monitor` / `alp explain` / `alp faultdecode` |
| Inspecting a generated artefact without building (Kconfig fragment, DTS overlay, system manifest, build plan) | `alp emit` (or `west alp-emit` from a west workspace) |
| Building, flashing, sizing, bundling, cleaning, or Renode-booting a project | `tan build` / `tan flash` / `tan size` / `tan image` / `tan clean` / `tan renode` -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) |
| A quick single-image native_sim or single-board build/flash loop without installing `tan` | `alp run` (native_sim by default; `--board <target>` for real hardware, `--flash` to program after building) |
| Scripting the surviving west-centric maintenance commands | `west alp-migrate` (board.yaml schema migration) / `west alp-lock` (dependency lockfile) / `west alp-quality` (quality-task registry) / `west alp-emit` (generated-artefact subset) |

Rules of thumb:

* `alp` doesn't own the plan-based, multi-slice build surface.  Every
  verb below is read-only (`emit`, `validate`, `doctor`, `explain`,
  `faultdecode`), a scaffold (`init`, `new-som`), a host-tool wrapper
  (`monitor`, `model`), or the retained single-image escape hatch
  (`run`).
* `tan` is the primary executor.  It consumes the SDK's
  `alp_orchestrate --emit build-plan` (and seeds its own
  `system-manifest.yaml` / `.alp-build-state.json` from
  `--emit system-manifest`), then drives `west` / `bitbake` / `cmake`
  per slice, owns skip-vs-fail policy, and programs hardware.  `tan
  build` (`--native` is the default, explicit opt-in) materialises the
  plan and runs each slice's build command directly, whatever
  `board.yaml` targets -- native_sim or real silicon; it never runs
  the produced binary itself (that's `tan run`).  See
  [heterogeneous-builds.md](heterogeneous-builds.md) for the per-core
  fan-out the plan describes.
* `alp emit` is a SUPERSET of `west alp-emit`: every artefact either
  front door can generate is reachable from `alp emit` (one catalog,
  listed in the `alp emit` verb reference below).  `west alp-emit`
  remains for west-centric scripting and
  exposes the orchestrator's ADR-0014 subset.  Same emitters
  underneath either way -- the two can never produce different output
  for the same mode.

## Verb reference

### `alp init` -- scaffold a new project

```bash
alp init my-app --som E1M-AEN801 --preset e1m-evk --peripherals uart,gpio
alp init my-app          # interactive: pick SoM / preset / peripherals
```

Copies the hello-world template, rewrites `board.yaml` for the chosen
SoM SKU + board preset, and seeds the optional starter `peripherals:`
list.  Omit any flag to be prompted interactively.

| Option | Meaning |
|---|---|
| `NAME` (argument) | New project directory (must not exist) |
| `--som` | SoM SKU, e.g. `E1M-AEN801` |
| `--preset` | Board preset from `metadata/boards/`, e.g. `e1m-evk` |
| `--peripherals` | Comma-separated starter peripherals (`uart,gpio,i2c,spi,pwm`) |

### `alp new-som` -- scaffold metadata for a new SoM port

```bash
alp new-som --sku E1M-NX9555 --soc-ref nxp:imx9:imx95 --family nxp-imx9
alp new-som               # interactive: prompts for every field
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

### Building, flashing, sizing, bundling, cleaning, Renode -- now `tan`

These used to be `alp build` / `alp flash` / `alp size` /
`alp image` / `alp clean` / `alp renode` (and their `west alp-*`
twins) -- `alp run` is the one survivor, retained as a single-image
escape hatch (see the verb reference above).  ADR-0020 Phase 4 retired
the SDK-side fan-out executor (`Orchestrator.fan_out()`,
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
`--emit system-manifest`.  It is independently versioned and
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

### `alp emit` -- print one generated artefact (no build)

```bash
alp emit zephyr-conf                   # the per-core Zephyr fragment
alp emit system-manifest               # the full-system manifest
alp emit hw-info-h --output hw_info.h  # write instead of stdout
alp emit zephyr-conf --core m55_he     # scope per-core modes to one core
alp emit build-plan                    # the orchestrator's build plan (JSON)
```

Read-only: shows exactly what a consuming tool (CMake, Yocto, the
IDE, or `tan`) would see.  This is the ONE catalog of every generated
artefact -- `alp emit` reaches everything `scripts/alp_project.py
--emit` and `west alp-emit` can produce, delegating each mode to the
single implementation that owns it (never a fork):

| Mode | Artefact | Emitted by |
|---|---|---|
| `zephyr-conf` | Per-core Zephyr `alp.conf` Kconfig fragment | `alp_project.py` |
| `cmake-args` | Per-core `-D` CMake argument list | `alp_project.py` |
| `yocto-conf` | Per-core `local.conf` fragment | `alp_project.py` |
| `dts-overlay` | Board DTS overlay (bus aliases + pin array) | `alp_project.py` |
| `native-sim-overlay` | native_sim overlay: `alp,pin-array` on `zephyr,gpio-emul` | `alp_project.py` |
| `hw-info-h` | Build-time `hw_info.h` macro header | `alp_project.py` |
| `west-libraries` | `west.yml` fragment for `libraries:` deps | `alp_project.py` |
| `system-manifest` | Full-system manifest (slices, boot order) | orchestrator |
| `dts-reservations` | DTS reserved-memory overlay (cross-core carve-outs) | orchestrator |
| `ipc-contract-h` | Cross-core IPC contract header | orchestrator |
| `os-topology` | Per-core natural-vs-effective OS facts | orchestrator |
| `composed-route-table` | JSON route-table dump (demonstrator) | `alp_project.py` |
| `carrier-netlist` | Studio-facing carrier nets + BOM JSON handoff | `alp_project.py` |
| `dts-partitions` | DTS fixed-partitions overlay (`storage:` entries) | orchestrator |
| `storage-mounts-c` | Static C storage mount table | orchestrator |
| `tfm-sysbuild-conf` | TF-M sysbuild child-image overlay (`security.psa:`) | orchestrator |
| `build-plan` | Per-slice build plan, JSON (IDE / CI / `tan` consumers) | orchestrator |

| Option | Meaning |
|---|---|
| `--input` | Path to `board.yaml` (default: nearest one upward) |
| `--output` | Write to this path instead of stdout |
| `--core` | Scope per-core modes to one core ID |
| `--build-root` | Build root used for `build-plan` slice paths |

`west alp-emit` exposes the orchestrator subset of the same catalog
(`system-manifest`, `ipc-contract-h`, `dts-reservations`,
`dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`,
`build-plan`) for west-centric scripting.

### `alp validate` -- check a board.yaml

```bash
alp validate                             # ./board.yaml, human output
alp validate path/to/board.yaml
alp validate --format json path/to/board.yaml    # IDE/LSP/CI-facing
alp validate --format sarif path/to/board.yaml   # SARIF 2.1.0 (code scanning)
```

Runs the rich diagnostic validator (JSON-Schema pass, SoM/preset
cross-references, peripheral-vs-SoC capability check), then the
same orchestrator consistency pass used by build preflight.
Exit code 0 means no hard errors; warnings such as ALP-B010 still
return 0.  Hard schema/xref/consistency errors return 1.

`--format` selects the rendering:

- `human` (default) -- the Rust-style block with an `ALP-Bxxx` code
  -- decode any code with `alp explain ALP-B001`.
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
the same rich validator plus consistency pass, so `alp validate`,
the script entry point, and build preflight reject the same
board.yaml contracts.

### `alp model` -- compile + package AI models

```bash
alp model build                          # compile board.yaml `models:` entries
alp model build --board path/to/board.yaml --out build/models
```

Compiles every `models:` entry declared in `board.yaml` into a
`.alpmodel` package via the SoM-appropriate backend (Vela for
Ethos-U, DRP-AI for RZ/V2N, ...).  See the model-pipeline docs under
`docs/tutorials/` for the end-to-end inference flow.

### `alp doctor` -- host environment preflight

```bash
alp doctor                     # human-readable PASS/WARN/FAIL report
alp doctor --json              # machine-readable
alp doctor --strict            # WARNs also fail the exit code
```

Strictly hardware-free: checks Python / west / CMake / Ninja / dtc /
gperf / imgtool / host compiler / J-Link presence, the Zephyr pin
(read live from `west.yml`), the workspace venv, `ZEPHYR_BASE`, the
Zephyr SDK, plus Windows-specific traps (git `core.autocrlf`,
long-path support).  Exit 0 = ready to build; every FAIL/WARN comes
with a remediation hint.  Run it first whenever a build machine
misbehaves.

### `alp monitor` -- serial console

<!-- cross-platform-lint:ignore -->
```bash
alp monitor --port COM7                       # Windows
alp monitor --port /dev/ttyUSB0               # Linux
alp monitor --port /dev/cu.usbserial-1420     # macOS
alp monitor                                    # lists available ports if none given
```
<!-- cross-platform-lint:resume -->

Opens pyserial's miniterm (Ctrl+] to quit).  Baud defaults to 115200
(`--baud` to override).  When no port is given or the requested one
doesn't exist, it lists every serial port on the host and exits
non-zero instead of hanging on a wrong device.

### `alp explain` -- decode a diagnostic code

```bash
alp explain ALP-B001
```

Prints the cause, fix, and doc link for any `ALP-Bxxx` validator
diagnostic (the codes `alp validate` emits).

### `alp faultdecode` -- decode a Cortex-M fault dump

```bash
alp faultdecode fault.txt
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
for the full flag reference.  `alp` itself never builds or flashes.

## Environment

| Variable | Effect |
|---|---|
| `ALP_SDK_ROOT` | Explicit path to the alp-sdk checkout; otherwise the CLI locates the repo it was installed (editable) from |
| `ZEPHYR_BASE` | The Zephyr tree checked by `alp doctor` |

`alp emit` exports `ALP_SDK_ROOT` and puts `<sdk>/scripts` on
`PYTHONPATH` for its sub-processes -- the same wiring `west alp-emit`
uses, so a CLI-invoked orchestrator run behaves identically to a west
one.  `tan` reads the plan's own `env` / `envAppendPath` entries
(sourced from the SDK's `--emit build-plan`) to set up
`EXTRA_ZEPHYR_MODULES` and `PYTHONPATH` for the slices it builds --
see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli).

## See also

- [getting-started.md](getting-started.md) -- install + first build.
- [heterogeneous-builds.md](heterogeneous-builds.md) -- the
  orchestrated multi-core pipeline `alp_orchestrate` plans and `tan
  build` fronts.
- [board-config-schema.md](board-config-schema.md) -- the `board.yaml`
  field reference `alp validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `alp doctor`
  isn't enough.
- [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) -- the
  standalone executor's own docs (`tan build` / `flash` / `size` /
  `image` / `clean` / `renode`).
