# The `tan` CLI

`tan` is the Alp SDK's single command-line front door: scaffold a
project, build it (multi-core aware), run it on the simulator or in
Renode, flash it, bundle it for OTA, clean it, inspect the generated
configuration, validate `board.yaml`, sanity-check the host
environment, and more -- all from one verb set.

`tan` is a standalone binary (the `tan-cli` project); it is not a
Python package. Under [ADR-0020](adr/0020-sdk-owns-build-execution.md)
the SDK is a **plans-only backend** -- it emits a build-plan and the
build libraries -- and `tan` is the **whole user-facing command
surface** that consumes that plan and drives the build. Point `tan` at
an SDK checkout with `--sdk-root <path>` (or `ALP_SDK_ROOT`), then set
up the toolchain once:

```bash
tan bootstrap        # west + Zephyr workspace + Python deps for the active SDK
tan sdk list         # manage local SDK installs (install / current / switch)
```

Every build verb is a **thin wrapper**: the actual logic lives in the
same scripts and packages the `west alp-*` extension commands drive
(`scripts/alp_orchestrate/`, `scripts/alp_project.py`,
`scripts/validate_board_yaml.py`, `scripts/flash_backends/`). `tan`
consumes the SDK's emitted build-plan and adds discoverability, sane
defaults, and a machine-readable JSON envelope; it never forks the
behaviour.

> **Coverage note.** `tan` wraps the full `west alp-*` extension surface
> (`build` / `flash` / `image` / `clean` / `renode` / `size` / `migrate` /
> `lock` / `quality`). A few operations are **not yet** in `tan` and still
> run through an SDK script: **`new-som`** (SoM porting), **`model`**
> (AI-model compile), **`monitor`** (serial console), and
> **`faultdecode`**. Their sections below are flagged; use the shown SDK
> form until `tan` grows the verb.

## `tan` vs `west alp-*` -- which one do I use?

Both front doors drive the **same underlying pipeline**; pick by
ergonomics, not capability:

| You are... | Use |
|---|---|
| In an app directory, want the shortest command | `tan build` / `tan build --native` / `tan flash` (they find `board.yaml` by walking up from the cwd) |
| Scripting CI or a west-centric workflow | `west alp-build <app>` / `west alp-flash <app>` (explicit app path, no cwd magic) |
| Outside any west workspace (validate, generate, doctor) | `tan ...` -- these verbs don't need a workspace |
| Building Zephyr for one named board target directly | either `tan build --board <target>` or plain `west build -b <target>` |

Rules of thumb:

* `tan build` == `west alp-build`: both pre-flight `board.yaml`
  through `scripts/validate_board_yaml.py`, then fan out every
  non-`off` core via `python -m alp_orchestrate` (per-core build
  slices + `build/system-manifest.yaml`). `tan build --plan` consumes
  the SDK's emitted `--emit build-plan` instead of re-deriving it. See
  [heterogeneous-builds.md](heterogeneous-builds.md).
* `tan flash` == `west alp-flash`: both walk
  `build/system-manifest.yaml` and hand every slice + helper MCU to
  its registered backend in `scripts/flash_backends/`, honouring the
  manifest's `boot_order:`.
* `tan generate` fronts the SDK's artefact emitters (`alp.conf`,
  DTS overlay, CMake args, Yocto conf) -- the same emitters
  `scripts/alp_project.py --emit` and `west alp-emit` own; the
  build-plan itself is emitted by the SDK
  (`alp_orchestrate.py --emit build-plan`) and consumed by
  `tan build --plan`.
* `tan clean` / `tan image` / `tan renode` ==
  `west alp-clean` / `west alp-image` / `west alp-renode`: same
  implementation, same flags; the `tan` form finds the app by walking
  up from the cwd, the west form takes the app path explicitly.
* `tan size` / `tan migrate` / `tan lock` / `tan quality` likewise wrap
  `west alp-size` / `west alp-migrate` / `west alp-lock` /
  `west alp-quality`.

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

### `alp new-som` -- scaffold metadata for a new SoM port

> **Not yet in `tan`.** SoM porting is a maintainer task run from the
> SDK checkout; the verb below is the SDK's porting scaffolder, pending
> a `tan` wrapper.

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

### `tan build` -- build the project (multi-core aware)

```bash
tan build                      # nearest board.yaml upward from the cwd
tan build path/to/app          # explicit app directory
tan build --core m33_sm        # iterate on one slice only
tan build --no-parallel        # sequential slice dispatch
tan build --board native_sim   # single-image fallback: plain `west build`
tan build --plan               # consume the SDK's emitted build-plan
```

Default path: validate `board.yaml`, then fan out every non-`off`
core via the orchestrator -- exactly what `west alp-build` does.
Outputs land under `<app>/build/`: per-slice build directories plus
`system-manifest.yaml` (the single source of truth that `tan flash`,
`tan image`, and OTA consume).

`--board <zephyr-board>` skips the orchestrator and performs a plain
single-image `west build -b <board>` into `build/<board>/` -- the
right tool for a one-core Zephyr app when you want to name the board
target directly. `--plan` (or `--plan-file <json>`) consumes the SDK's
`--emit build-plan` rather than invoking the SDK to re-derive it.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--core` | Limit the fan-out to one core ID |
| `--native` | Build + run the native_sim image (the old `alp run`) |
| `--no-parallel` | Force sequential slice dispatch |
| `--no-validate` | Skip the `board.yaml` pre-flight validation |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--board` | Single-image fallback: `west build -b <board>` |
| `--plan` / `--plan-file` | Consume the SDK's emitted build-plan JSON |

### `tan build --native` -- build + run on the simulator

```bash
tan build --native             # build for native_sim and execute it
tan build --board <target>     # real-hardware build instead
tan build --board <target> --flash   # ...then flash
```

Builds through the same single-image path as `tan build --board` and
then executes the produced `zephyr.exe` (native_sim).  For
heterogeneous multi-core projects use `tan build` + `tan flash`
instead -- native_sim is a single-image target.

### `tan flash` -- program the built project

```bash
tan build && tan flash                     # the whole system, one-liner
tan flash --core m33_sm                    # one slice only
tan flash --helper gd32_bridge             # one helper MCU only
tan flash --dry-run                        # print commands, touch nothing
tan flash --skip-missing-tools             # warn+skip when a tool is absent
```

Walks `build/system-manifest.yaml` (produced by `tan build` /
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

### `tan size` -- footprint vs the SoM memory budget

```bash
tan build && tan size            # does each image fit the silicon?
tan size --fail-over-budget      # CI gate: non-zero exit when over
tan size --json                  # machine-readable (VS Code extension)
```

Walks `build/system-manifest.yaml` (produced by `tan build` /
`west alp-build`), measures every Zephyr slice's `zephyr.elf`, and
compares FLASH and RAM usage against the budget resolved from the
SoM's SoC metadata -- BEFORE you flash.
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

### `tan image` -- assemble a flashable bundle

```bash
tan build && tan image           # produces build/image-bundle/
```

Reads `build/system-manifest.yaml` and assembles a single flashable
bundle under `build/image-bundle/`: per-slice `tar.gz` archives,
helper-MCU firmware, and a `bundle-manifest.json` with per-artefact
SHA-256s.  Identical to `west alp-image`.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |

### `tan clean` -- remove build outputs

```bash
tan clean                        # rm -rf <app>/build + orchestrator cache
tan clean --dry-run              # list what would go, delete nothing
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

### `tan renode` -- boot the build in Renode (no hardware)

```bash
tan build && tan renode                      # headless smoke boot
tan renode --expect "[hello] done"           # exit 0 when seen, 1 if not
tan renode --log out.log --timeout 60
tan renode --sim-mode --board E1M-V2N101 \   # studio hardware simulator
    --image-bundle ./bundle                  #   (issue #674)
```

Boots the built system manifest's Zephyr slice in the
[Renode](https://renode.io) emulator, headless, with a wall-clock
timeout -- a no-hardware smoke test.  The SoM family maps to a
platform descriptor under `metadata/renode/`; the console output tees
to `--log` (default `build/renode.log`).  Requires the `renode` binary
on PATH -- exits non-zero with install guidance when absent, never a
silent pass.  Identical to `west alp-renode`.

**`--sim-mode`** (studio hardware-simulator contract, issue #674): instead
of the smoke boot, boots the `--image-bundle` firmware and exposes it to
alp-studio's sim gateway.  It writes `<bundle>/sim-descriptor.json` (an
`@alp/sim-protocol` `SimDescriptorSchema` document), streams the firmware
console over a TCP socket, and serves a line-oriented control socket
(`sysbus ReadBytes`/`WriteBytes` + the descriptor's peripheral inject
templates) until `--timeout`.  First target: **E1M-V2N101** (RZ/V2N
M33-SM, tmp112 over I2C).

The M33-SM is a **headless** core — no hardware UART console (the board
sets `CONFIG_CONSOLE=n`; the console is the A55's).  So `--sim-mode`
observes its console the way alp-sdk does on the bench: it polls the
firmware's Zephyr `ram_console_buf` RAM ring out of SRAM and streams it to
the UART socket.  Build a headless image with the `ram_console` backend
(see `tests/renode/v2n_m33_ramconsole.conf`) for a console to stream; a
truly silent firmware just leaves the UART socket quiet.  The faithful
`renesas_rzv2n` Renode model boots the real V2N M33 image (SRAM at
`0x08003000`, a coarse CPG stub for the FSP clock/reset handshakes, and
`iic8`/tmp112); no custom hardware-UART model is involved.

Also supported: **E1M-AEN801** (Alif Ensemble E8, Cortex-M55-HP) — which
*does* have a wired **UART5 console**, so its board profile streams that
real UART over a Renode socket terminal (not `ram_console`). Two AEN
specifics: it needs Renode **≥ 1.16.1** (Cortex-M55 + Helium support), and
the sim image must be **ITCM-linked** (`chosen zephyr,flash = &itcm`, see
`tests/renode/aen_m55_itcm_run.overlay`) so its vector table sits at the
low ITCM address `0x0` — Renode 1.16.1 mis-executes a vector table placed
at the high MRAM base `0x80000000`. The `alif_ensemble_e8` model maps ITCM
at `0x0` and stubs the Alif CGU/EXPMST clock registers.

| Option | Meaning |
|---|---|
| `APP_PATH` (argument, optional) | App directory (default: walk up from cwd) |
| `--build-root` | Override the build root (default: `<app>/build`) |
| `--board` | Override the SoM SKU used to pick the platform descriptor |
| `--log` | Tee the console output to this file |
| `--timeout` | Wall-clock cap in seconds (default 120) |
| `--expect` | Stop early (exit 0) when this substring appears; exit 1 if it never does |
| `--image-bundle` | Directory of pre-built artefacts; the firmware ELF source for `--sim-mode` |
| `--sim-mode` | Studio hardware-simulator mode: emit `sim-descriptor.json` + serve the UART/control sockets |

### `tan generate` -- generate a build artefact (no build)

```bash
tan generate --target zephyr-conf              # the per-core Zephyr fragment
tan generate --target dts-overlay              # board DTS overlay
tan generate --target cmake-args               # per-core -D CMake args
tan generate --target yocto-conf               # per-core local.conf fragment
```

Read-only: shows exactly what a consuming tool (CMake, Yocto, the
IDE) would see. `tan generate` fronts the SDK's `board.yaml` emitters
(the same ones `scripts/alp_project.py --emit` and `west alp-emit`
own), delegating each target to the single implementation that owns it
(never a fork). The full-system plan is a separate concern: the SDK
emits it with `alp_orchestrate.py --emit build-plan` and `tan build
--plan` consumes it.

| Target | Artefact | Emitted by |
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

### `alp model` -- compile + package AI models

> **Not yet in `tan`.** The model-compile front-end still runs through
> the SDK; a `tan model` wrapper is pending.

```bash
alp model build                          # compile board.yaml `models:` entries
alp model build --board path/to/board.yaml --out build/models
```

Compiles every `models:` entry declared in `board.yaml` into a
`.alpmodel` package via the SoM-appropriate backend (Vela for
Ethos-U, DRP-AI for RZ/V2N, ...).  See the model-pipeline docs under
`docs/tutorials/` for the end-to-end inference flow.

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

### `alp monitor` -- serial console

> **Not yet in `tan`.** The serial console still runs through the SDK's
> `alp monitor`; a `tan monitor` wrapper is pending.

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

### `tan explain` -- decode a diagnostic code

```bash
tan explain ALP-B001
```

Prints the cause, fix, and doc link for any `ALP-Bxxx` validator
diagnostic (the codes `tan validate` emits).

### `alp faultdecode` -- decode a Cortex-M fault dump

> **Not yet in `tan`.** Fault decoding still runs through the SDK's
> `alp faultdecode`; a `tan faultdecode` wrapper is pending.

```bash
alp faultdecode fault.txt
```

Decodes an ARMv8-M (M33/M55) fault-register dump into a
human-readable cause chain.

## One-liner flash examples

```bash
# AEN (E1M-AEN801 M55): build every slice + flash over SWD, one line.
west alp-build examples/peripheral-io/gpio-button-led && west alp-flash examples/peripheral-io/gpio-button-led

# The same, from inside the app directory, via the tan CLI:
cd examples/peripheral-io/gpio-button-led && tan build && tan flash

# V2N helper MCU only (GD32 bridge), preview first:
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge --dry-run
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge

# One slice of a heterogeneous system:
west alp-flash examples/multicore/rpmsg-v2n --core m33_sm
```

## Environment

| Variable | Effect |
|---|---|
| `ALP_SDK_ROOT` | Explicit path to the alp-sdk checkout (equivalent to `--sdk-root`); otherwise `tan` resolves the SDK from the project tree / workspace venv |
| `ZEPHYR_BASE` | The Zephyr tree used by single-image builds + checked by `tan doctor` |

The delegating verbs (build, flash, generate, image, clean, renode)
export `ALP_SDK_ROOT`, append the SDK to `EXTRA_ZEPHYR_MODULES`, and
put `<sdk>/scripts` on `PYTHONPATH` for their sub-processes -- the
same wiring the `west alp-*` wrappers use, so a `tan`-invoked
orchestrator run behaves identically to a west one.

## See also

- [getting-started.md](getting-started.md) -- install + first build.
- [heterogeneous-builds.md](heterogeneous-builds.md) -- the
  orchestrated multi-core pipeline `tan build` fronts.
- [board-config-schema.md](board-config-schema.md) -- the `board.yaml`
  field reference `tan validate` enforces.
- [troubleshooting.md](troubleshooting.md) -- when `tan doctor`
  isn't enough.
