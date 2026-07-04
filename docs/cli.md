# The `alp` CLI

`alp` is the Alp SDK's single command-line front door: scaffold a
project, build it (multi-core aware), run it on the simulator or in
Renode, check its footprint against the SoM memory budget, flash it,
bundle it for OTA, clean it, inspect the generated configuration,
validate `board.yaml`, compile AI models, sanity-check the host
environment, open a serial console, and scaffold the metadata for
porting a new SoM -- all from one verb set.

It is installed automatically by the bootstrap scripts
(`scripts/bootstrap.sh` on Linux/macOS/WSL2, `scripts/bootstrap.ps1`
on native Windows) as an editable install into the workspace venv, so
a `git pull` in the alp-sdk checkout updates the CLI in place.
Without an install it also runs straight from a checkout:

```bash
# From the alp-sdk repo root (or with <sdk>/scripts on PYTHONPATH):
PYTHONPATH=scripts python3 -m alp_cli --help
```

Every verb is a **thin wrapper**: the actual logic lives in the same
scripts and packages the `west alp-*` extension commands drive
(`scripts/alp_orchestrate/`, `scripts/alp_project.py`,
`scripts/validate_board_yaml.py`, `scripts/flash_backends/`).  The CLI
adds discoverability and sane defaults; it never forks the behaviour.

## `alp` vs `west alp-*` -- which one do I use?

Both front doors drive the **same underlying pipeline**; pick by
ergonomics, not capability:

| You are... | Use |
|---|---|
| In an app directory, want the shortest command | `alp build` / `alp run` / `alp flash` (they find `board.yaml` by walking up from the cwd) |
| Scripting CI or a west-centric workflow | `west alp-build <app>` / `west alp-flash <app>` (explicit app path, no cwd magic) |
| Outside any west workspace (validate, emit, doctor, model, monitor) | `alp ...` -- these verbs don't need a workspace |
| Building Zephyr for one named board target directly | either `alp build --board <target>` or plain `west build -b <target>` |

Rules of thumb:

* `alp build` == `west alp-build`: both pre-flight `board.yaml`
  through `scripts/validate_board_yaml.py`, then fan out every
  non-`off` core via `python -m alp_orchestrate` (per-core build
  slices + `build/system-manifest.yaml`).  See
  [heterogeneous-builds.md](heterogeneous-builds.md).
* `alp flash` == `west alp-flash`: both walk
  `build/system-manifest.yaml` and hand every slice + helper MCU to
  its registered backend in `scripts/flash_backends/`, honouring the
  manifest's `boot_order:`.
* `alp emit` is a SUPERSET of `west alp-emit`: every artefact either
  front door can generate is reachable from `alp emit` (one catalog,
  listed in the `alp emit` verb reference below).  `west alp-emit`
  remains for west-centric scripting and
  exposes the orchestrator's ADR-0014 subset.  Same emitters
  underneath either way -- the two can never produce different output
  for the same mode.
* `alp size` / `alp clean` / `alp image` / `alp renode` ==
  `west alp-size` / `west alp-clean` / `west alp-image` /
  `west alp-renode`: same implementation, same flags; the `alp` form
  finds the app by walking up from the cwd, the west form takes the
  app path explicitly.

## Verb reference

### `alp init` -- scaffold a new project

```bash
alp init my-app --som E1M-AEN701 --preset e1m-evk --peripherals uart,gpio
alp init my-app          # interactive: pick SoM / preset / peripherals
```

Copies the hello-world template, rewrites `board.yaml` for the chosen
SoM SKU + board preset, and seeds the optional starter `peripherals:`
list.  Omit any flag to be prompted interactively.

| Option | Meaning |
|---|---|
| `NAME` (argument) | New project directory (must not exist) |
| `--som` | SoM SKU, e.g. `E1M-AEN701` |
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

### `alp build` -- build the project (multi-core aware)

```bash
alp build                      # nearest board.yaml upward from the cwd
alp build path/to/app          # explicit app directory
alp build --core m33_sm        # iterate on one slice only
alp build --no-parallel        # sequential slice dispatch
alp build --board native_sim   # single-image fallback: plain `west build`
```

Default path: validate `board.yaml`, then fan out every non-`off`
core via the orchestrator -- exactly what `west alp-build` does.
Outputs land under `<app>/build/`: per-slice build directories plus
`system-manifest.yaml` (the single source of truth that `alp flash`,
`west alp-image`, and OTA consume).

`--board <zephyr-board>` skips the orchestrator and performs a plain
single-image `west build -b <board>` into `build/<board>/` -- the
right tool for a one-core Zephyr app when you want to name the board
target directly.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--core` | Limit the fan-out to one core ID |
| `--no-parallel` | Force sequential slice dispatch |
| `--no-validate` | Skip the `board.yaml` pre-flight validation |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--board` | Single-image fallback: `west build -b <board>` |

### `alp run` -- build + run on the simulator

```bash
alp run                        # build for native_sim and execute it
alp run --board <target>       # real-hardware build instead
alp run --board <target> --flash   # ...then west flash
```

Builds through the same single-image path as `alp build --board` and
then executes the produced `zephyr.exe` (native_sim).  For
heterogeneous multi-core projects use `alp build` + `alp flash`
instead -- native_sim is a single-image target.

### `alp flash` -- program the built project

```bash
alp build && alp flash                     # the whole system, one-liner
alp flash --core m33_sm                    # one slice only
alp flash --helper gd32_bridge             # one helper MCU only
alp flash --dry-run                        # print commands, touch nothing
alp flash --skip-missing-tools             # warn+skip when a tool is absent
```

Walks `build/system-manifest.yaml` (produced by `alp build` /
`west alp-build`) and programs every slice + helper MCU via the
registered flash backends, in `boot_order:`.  Identical to
`west alp-flash`; the west form takes the app path explicitly:

```bash
west alp-build examples/multicore/rpmsg-v2n && west alp-flash examples/multicore/rpmsg-v2n
```

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--core` | Flash only the slice with this core ID |
| `--helper` | Flash only the helper MCU with this name |
| `--dry-run` | Print the flash commands but don't run them |
| `--build-root` | Override the build root |
| `--skip-missing-tools` | Warn + skip slices whose flash tool is missing |

### `alp size` -- footprint vs the SoM memory budget

```bash
alp build && alp size            # does each image fit the silicon?
alp size --fail-over-budget      # CI gate: non-zero exit when over
alp size --json                  # machine-readable (VS Code extension)
```

Walks `build/system-manifest.yaml` (produced by `alp build` /
`west alp-build`), measures every Zephyr slice's `zephyr.elf`, and
compares FLASH and RAM usage against the budget resolved from the
SoM's SoC metadata -- BEFORE you flash.  Identical to `west alp-size`.
Slices that aren't built are reported as `not built`, Yocto/baremetal
slices as `n/a`, and an unresolvable budget shows `unknown` (never a
guessed number; `--fail-over-budget` skips such slices and says so).

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--board` | Override the SoM SKU used to resolve the budget |
| `--json` | Machine-readable report instead of the human table |
| `--fail-over-budget` | Exit non-zero if any slice exceeds its budget |

### `alp image` -- assemble a flashable bundle

```bash
alp build && alp image           # produces build/image-bundle/
```

Reads `build/system-manifest.yaml` and assembles a single flashable
bundle under `build/image-bundle/`: per-slice `tar.gz` archives,
helper-MCU firmware, and a `bundle-manifest.json` with per-artefact
SHA-256s.  Identical to `west alp-image`.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |

### `alp clean` -- remove build outputs

```bash
alp clean                        # rm -rf <app>/build + orchestrator cache
alp clean --dry-run              # list what would go, delete nothing
```

Removes the per-project build directory and the orchestrator's
`.alp-build-state.json`.  The per-slice build dirs are self-contained,
so no Zephyr/Yocto-specific cleaners are needed.  Identical to
`west alp-clean`.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--dry-run` | List paths that would be removed, don't delete |

### `alp renode` -- boot the build in Renode (no hardware)

```bash
alp build && alp renode                      # headless smoke boot
alp renode --expect "[hello] done"           # exit 0 when seen, 1 if not
alp renode --log out.log --timeout 60
```

Boots the built system manifest's Zephyr slice in the
[Renode](https://renode.io) emulator, headless, with a wall-clock
timeout -- a no-hardware smoke test.  The SoM family maps to a
platform descriptor under `metadata/renode/`; the console output tees
to `--log` (default `build/renode.log`).  Requires the `renode` binary
on PATH -- exits non-zero with install guidance when absent, never a
silent pass.  Identical to `west alp-renode`.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--board` | Override the SoM SKU used to pick the platform descriptor |
| `--log` | Tee the console output to this file |
| `--timeout` | Wall-clock cap in seconds (default 120) |
| `--expect` | Stop early (exit 0) when this substring appears; exit 1 if it never does |

### `alp emit` -- print one generated artefact (no build)

```bash
alp emit zephyr-conf                   # the per-core Zephyr fragment
alp emit system-manifest               # the full-system manifest
alp emit hw-info-h --output hw_info.h  # write instead of stdout
alp emit zephyr-conf --core m55_he     # scope per-core modes to one core
alp emit build-plan                    # the orchestrator's build plan (JSON)
```

Read-only: shows exactly what a consuming tool (CMake, Yocto, the
IDE) would see.  This is the ONE catalog of every generated artefact
-- `alp emit` reaches everything `scripts/alp_project.py --emit` and
`west alp-emit` can produce, delegating each mode to the single
implementation that owns it (never a fork):

| Mode | Artefact | Emitted by |
|---|---|---|
| `zephyr-conf` | Per-core Zephyr `alp.conf` Kconfig fragment | `alp_project.py` |
| `cmake-args` | Per-core `-D` CMake argument list | `alp_project.py` |
| `yocto-conf` | Per-core `local.conf` fragment | `alp_project.py` |
| `dts-overlay` | Board DTS overlay (bus aliases + pin array) | `alp_project.py` |
| `hw-info-h` | Build-time `hw_info.h` macro header | `alp_project.py` |
| `west-libraries` | `west.yml` fragment for `libraries:` deps | `alp_project.py` |
| `system-manifest` | Full-system manifest (slices, boot order) | orchestrator |
| `dts-reservations` | DTS reserved-memory overlay (cross-core carve-outs) | orchestrator |
| `ipc-contract-h` | Cross-core IPC contract header | orchestrator |
| `os-topology` | Per-core natural-vs-effective OS facts | orchestrator |
| `composed-route-table` | JSON route-table dump (demonstrator) | `alp_project.py` |
| `dts-partitions` | DTS fixed-partitions overlay (`storage:` entries) | orchestrator |
| `storage-mounts-c` | Static C storage mount table | orchestrator |
| `tfm-sysbuild-conf` | TF-M sysbuild child-image overlay (`security.psa:`) | orchestrator |
| `build-plan` | Per-slice build plan, JSON (IDE / CI consumers) | orchestrator |

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
alp validate                   # ./board.yaml
alp validate path/to/board.yaml
```

Runs the rich diagnostic validator (JSON-Schema pass, SoM/preset
cross-references, peripheral-vs-SoC capability check) and renders
every finding as a Rust-style diagnostic block with an `ALP-Bxxx`
code -- decode any code with `alp explain ALP-B001`.  Exit code 0
when clean, 1 on errors.

The schema pass is the SAME shared implementation used by
`scripts/validate_board_yaml.py` (the `west alp-build` pre-flight)
and the orchestrator's loader, so all front doors report identical
violations.

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

```bash
alp monitor --port COM7                # Windows
alp monitor --port /dev/ttyUSB0       # Linux
alp monitor                            # lists available ports if none given
```

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

## One-liner flash examples

```bash
# AEN (E1M-AEN801 M55): build every slice + flash over SWD, one line.
west alp-build examples/peripheral-io/gpio-button-led && west alp-flash examples/peripheral-io/gpio-button-led

# The same, from inside the app directory, via the alp CLI:
cd examples/peripheral-io/gpio-button-led && alp build && alp flash

# V2N helper MCU only (GD32 bridge), preview first:
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge --dry-run
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge

# One slice of a heterogeneous system:
west alp-flash examples/multicore/rpmsg-v2n --core m33_sm
```

## Environment

| Variable | Effect |
|---|---|
| `ALP_SDK_ROOT` | Explicit path to the alp-sdk checkout; otherwise the CLI locates the repo it was installed (editable) from |
| `ZEPHYR_BASE` | The Zephyr tree used by single-image builds + checked by `alp doctor` |

The delegating verbs (build, flash, emit, size, image, clean, renode)
export `ALP_SDK_ROOT`, append the SDK to `EXTRA_ZEPHYR_MODULES`, and
put `<sdk>/scripts` on `PYTHONPATH` for their sub-processes -- the
same wiring the `west alp-*` wrappers use, so a CLI-invoked
orchestrator run behaves identically to a west one.

## See also

- [getting-started.md](getting-started.md) -- install + first build.
- [heterogeneous-builds.md](heterogeneous-builds.md) -- the
  orchestrated multi-core pipeline `alp build` fronts.
- [board-config.md](board-config.md) -- the `board.yaml` v2 reference
  `alp validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `alp doctor`
  isn't enough.
