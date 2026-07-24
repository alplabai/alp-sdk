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
separately (needs a Rust toolchain / rustup):

```bash
cargo install --git https://github.com/alplabai/tan-cli --bin tan
```

Two execution paths live behind the one binary:

* **Build / flash / size / image / clean / renode** -- `tan`'s own
  primary build executor.  Per ADR
  [0020](adr/0020-sdk-owns-build-execution.md), alp-sdk is
  **plans-only** for this surface: `tan` consumes
  `alp_orchestrate --emit build-plan` / `--emit system-manifest` and
  runs `west` / `bitbake` / `cmake` per slice itself.
* **Everything else** (`init`, `new-som`, `validate`, `model`,
  `doctor`, `monitor`, `explain`, `faultdecode`, `emit`, `run`) --
  `tan` forwards to the SDK's Python backend as `python -m alp_cli
  <sub>`.  No `alp` binary is installed anywhere -- `pyproject.toml`
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
| Scaffolding a project, validating `board.yaml`, compiling a model, checking your host, opening a serial console, decoding a diagnostic/fault, or running a quick single-image native_sim/single-board loop | `tan init` / `tan new-som` / `tan validate` / `tan model` / `tan doctor` / `tan monitor` / `tan explain` / `tan faultdecode` / `tan run` |
| Inspecting a generated artefact without building (Kconfig fragment, DTS overlay, system manifest, build plan) | `tan emit` (or `west alp-emit` from a west workspace) |
| Building, flashing, sizing, bundling, cleaning, or Renode-booting a project | `tan build` / `tan flash` / `tan size` / `tan image` / `tan clean` / `tan renode` -- see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) |
| Scripting the surviving west-centric maintenance commands | `west alp-migrate` (board.yaml schema migration) / `west alp-lock` (dependency lockfile) / `west alp-quality` (quality-task registry) / `west alp-emit` (generated-artefact subset) |

Rules of thumb:

* `tan` is the SDK's sole user-facing front door for both the
  plan-based, multi-slice build surface and every scaffold / validate
  / inspect / host-tool verb (ADR
  [0020](adr/0020-sdk-owns-build-execution.md), end-state B).  The
  non-build verbs (`emit`, `validate`, `doctor`, `explain`,
  `faultdecode`, `init`, `new-som`, `monitor`, `model`, `run`) forward
  to the Python backend; alp-sdk itself never runs them directly.
* For build/flash: `tan` consumes the SDK's
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
* `tan emit` is a SUPERSET of `west alp-emit`: every artefact either
  front door can generate is reachable from `tan emit` (one catalog,
  listed in the `tan emit` verb reference below).  `west alp-emit`
  remains for west-centric scripting and
  exposes the orchestrator's ADR-0014 subset.  Same emitters
  underneath either way -- the two can never produce different output
  for the same mode.

## Verb reference

### `tan init` -- scaffold a new project

```bash
tan init my-app --som E1M-AEN801 --preset e1m-evk --peripherals uart,gpio
tan init my-app          # interactive: pick SoM / preset / peripherals
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

### `tan emit` -- print one generated artefact (no build)

```bash
tan emit zephyr-conf                   # the per-core Zephyr fragment
tan emit system-manifest               # the full-system manifest
tan emit hw-info-h --output hw_info.h  # write instead of stdout
tan emit zephyr-conf --core m55_he     # scope per-core modes to one core
tan emit build-plan                    # the orchestrator's build plan (JSON)
tan emit kconfig --core m55_he         # board-scoped Kconfig symbol menu (needs ZEPHYR_BASE)
tan emit scaffold --template minimal --sku E1M-V2N101  # new-project files, no board.yaml needed
```

Read-only: shows exactly what a consuming tool (CMake, Yocto, the
IDE, or `tan`) would see.  This is the ONE catalog of every generated
artefact -- `tan emit` reaches everything `scripts/alp_project.py
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
| `scaffold` | New-project `{path, contents}[]` envelope for a template (`--template`/`--sku`) | `alp_project.py` |
| `dts-partitions` | DTS fixed-partitions overlay (`storage:` entries) | orchestrator |
| `storage-mounts-c` | Static C storage mount table | orchestrator |
| `tfm-sysbuild-conf` | TF-M sysbuild child-image overlay (`security.psa:`) | orchestrator |
| `build-plan` | Per-slice build plan, JSON (IDE / CI / `tan` consumers) | orchestrator |
| `kconfig` | Board-scoped, user-settable Kconfig symbol menu for one `--core <id>` (the vscode `prj.conf` LSP's live feed) | orchestrator (**workspace-dependent** -- see below) |

| Option | Meaning |
|---|---|
| `--input` | Path to `board.yaml` (default: nearest one upward) |
| `--output` | Write to this path instead of stdout |
| `--core` | Scope per-core modes to one core ID |
| `--build-root` | Build root used for `build-plan` slice paths |
| `--template` | Template catalog id (`metadata/templates/catalog-v1.json`); required for `scaffold` |
| `--sku` | Target SoM SKU; required for `scaffold` (must be one of the template's `supported.som_skus`) |

`west alp-emit` exposes the orchestrator subset of the same catalog
(`system-manifest`, `ipc-contract-h`, `dts-reservations`,
`dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`,
`build-plan`, `kconfig`) for west-centric scripting.

#### `--emit kconfig` -- the SDK's first workspace-dependent emit

Every other `--emit` mode is hermetic (provable from `board.yaml` +
this repo's own metadata alone, with no Zephyr toolchain on disk --
see `scripts/check_emit_snapshots.py`, the byte-golden gate that pins
all of them). `--emit kconfig` is deliberately the first exception:
it needs a bootstrapped Zephyr workspace (`ZEPHYR_BASE`, the SDK's
pinned v4.4.0) because only the real Kconfig solver knows which
symbols are user-promptable for a given board -- the SDK doesn't (and
won't) re-implement Kconfig's dependency/visibility engine itself.

```bash
python -m alp_orchestrate --input board.yaml --emit kconfig --core m55_he
# also: tan emit kconfig --core m55_he / west alp-emit kconfig --core m55_he
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

Unblocks tan-cli [#35](https://github.com/alplabai/tan-cli/issues/35)
(`tan kconfig`, wrapping this emit in `Envelope<KconfigData>`) and a
follow-up alp-sdk-vscode change to point the `prj.conf` LSP's symbol
menu at this live feed instead of its hand-vendored snapshot -- both
out of scope for this change.

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

#### `tan model check` -- static pre-flight fit/perf check (no toolchain)

```bash
tan model check my_model.tflite --sku E1M-AEN801
tan model check my_model.tflite --sku E1M-AEN801 --format json
tan model check my_model.tflite --sku E1M-V2N101 --metadata-root path/to/metadata
```

Offline, no-toolchain static analysis: parses the TFLite graph and,
for every backend the SKU exposes (`cpu` plus each on-SoM NPU),
answers "will this model fit, and roughly how fast" *before* any
compile.  Each backend gets a fit verdict -- `fits` / `cpu-fallback`
(one or more ops fall back to CPU) / `no-fit` (SRAM arena overflow) --
plus conservative `est_sram_kib` / `est_latency_ms` /
`op_coverage_pct` estimates.  `--sku` is required (e.g.
`E1M-AEN801`); `--metadata-root` overrides the SDK's own `metadata/`
root (defaults to it).  Exit 0 for any completed analysis -- a
`no-fit` verdict is a valid answer, not a CLI error; exit 1 for an
unknown SKU or a non-TFLite model (ONNX static analysis is a
follow-on, not yet supported); a missing/unreadable model path is a
usage error (exit 2), same as any other `tan model` path argument.

`--format json` payload:

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

### `tan doctor` -- host environment preflight

```bash
tan doctor                     # human-readable PASS/WARN/FAIL report
tan doctor --json              # machine-readable
tan doctor --strict            # WARNs also fail the exit code
```

Strictly hardware-free: checks Python / west / CMake / Ninja / dtc /
gperf / imgtool / host compiler / J-Link presence, the Zephyr pin
(read live from `west.yml`), the workspace venv, `ZEPHYR_BASE`, the
Zephyr SDK, plus Windows-specific traps (git `core.autocrlf`,
long-path support).  Exit 0 = ready to build; every FAIL/WARN comes
with a remediation hint.  Run it first whenever a build machine
misbehaves.

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
| `ZEPHYR_BASE` | The Zephyr tree checked by `tan doctor` |

`tan emit` exports `ALP_SDK_ROOT` and puts `<sdk>/scripts` on
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
  field reference `tan validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `tan doctor`
  isn't enough.
- [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli) -- the
  standalone executor's own docs (`tan build` / `flash` / `size` /
  `image` / `clean` / `renode`).
