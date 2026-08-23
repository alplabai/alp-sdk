# Getting started with the Alp SDK

This walkthrough takes you from "git clone" to a working
`gpio-button-led` build, cross-compiled for its real E1M-AEN801
target.  No `alp-studio` required — the SDK supports hand-written
firmware as a first-class consumer.

> **Rendered version:** the full SDK documentation site lives at
> [**docs.alplab.ai/sdk/introduction**](https://docs.alplab.ai/sdk/introduction).
> This in-repo markdown is the source of truth; the site mirrors
> it with cross-version navigation + search.  Stuck on something?
> Ask on [**community.alplab.ai**](https://community.alplab.ai/).

> **One user CLI: `tan`.** The SDK's `board.yaml` remains the project source of
> truth. Python Tan owns the normal in-process planner and executor; alp-sdk
> owns the metadata, schemas, generated contracts, and reference emitters used
> to keep that relocated planner in parity (ADR
> [0020](adr/0020-sdk-owns-build-execution.md)).
>
> - **`tan` CLI** — the planner and build executor, in a standalone public repo
>   ([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)).
>   `tan build --project <app-dir>` fans the app's `board.yaml` out
>   into per-core build slices in process, runs the full pre-flight (schema validation, SoC
>   caps, hw_info header) and delegates to `west build` for the real
>   SoM `board.yaml` names; there is no separate `--board`/`--core`
>   selector, the target comes from the project itself.  This is the headline
>   [README Quickstart](../README.md#quickstart) — if you just want
>   a hello-world running in two minutes, start there. As of `tan-cli`
>   [v0.5.0](https://github.com/alplabai/tan-cli/releases/tag/v0.5.0) (current
>   release: [v0.5.1](https://github.com/alplabai/tan-cli/releases/tag/v0.5.1)),
>   the published release installer (`curl -fsSL
>   https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh | sh`
>   on Linux/macOS, `irm
>   https://raw.githubusercontent.com/alplabai/tan-cli/main/install.ps1 | iex`
>   on Windows) installs the real
>   Python `tan` directly — it no longer resolves the frozen Rust v0.4.1
>   release. Four prebuilt archives ship (Windows x64, Linux x64 glibc, macOS
>   x64/arm64); other hosts install from source. Most users should just run
>   the release installer above and skip the "fastest path" block below —
>   this walkthrough's own source-build steps exist to exercise the exact
>   clone-and-build path this repo's own CI validates (and are how you'd
>   pick up a `tan-cli` fix that hasn't been tagged yet), not because the
>   release is behind. Alp's `tan` is
>   not distributed on PyPI, and the bare name `tan` there belongs to an
>   unrelated project (`200` for it: `tan` v23.7.0, "The compromising code
>   formatter") -- `pip install tan` does not get you this tool. `alp-tan`
>   is not registered there either (`404` for it, not a reservation
>   placeholder).
> - **Native Python commands** — `tan
>   init` scaffolds a project, `tan validate` checks a `board.yaml`,
>   `tan generate --target zephyr-conf` (one of twelve supported targets
>   — see [`docs/cli.md`](cli.md) for the full list) regenerates a
>   single build artefact, plus `tan monitor` / `tan model` /
>   `tan new-som` / `tan explain` / `tan faultdecode` / `tan doctor`.
>   Only `migrate`, `lock`, and `quality` still forward to west commands.
>   The full verb reference lives in [`docs/cli.md`](cli.md).
>
> Every `board.yaml`-driven `tan build --project <app-dir>` runs
> through the same loader and validator, whichever real SoM the
> project targets.  The surviving west-extension commands (`west
> alp-migrate`, `west alp-lock`, `west alp-quality`, `west alp-emit`)
> are unaffected by the build-executor move.

If you'd rather skim, here's the source-build path in one block (the
release installer above is the faster route if you just want the tool —
this is the clone-and-build path CI itself exercises):

> Needs `git` and Python 3.12+ already on PATH -- a from-scratch host (a
> bare container, a fresh VM) needs those installed first; see the
> per-platform one-liners under "1. Prerequisites" below
> (`sudo apt install -y git curl` on Debian/Ubuntu).

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk
git clone --branch dev https://github.com/alplabai/tan-cli ../tan-cli
python3 -m venv ../tan-cli/.venv
../tan-cli/.venv/bin/python -m pip install ../tan-cli/python
export PATH="$PWD/../tan-cli/.venv/bin:$PATH"
tan bootstrap --sdk-root "$PWD"                      # one-time: west + Zephyr workspace + venv

# one-time: the arm-zephyr-eabi cross toolchain this build needs (~a few
# hundred MB with --no-hosttools) -- bootstrap.sh does not install this
# for you, only the west/Python layer above it
( cd .. && west sdk install --gnu-toolchains arm-zephyr-eabi --no-hosttools \
    --install-dir "$PWD/zephyr-sdk" )
export ZEPHYR_SDK_INSTALL_DIR="$PWD/../zephyr-sdk"

# sdk install/switch are not yet ported; select this checkout explicitly.
tan build --project examples/peripheral-io/gpio-button-led --sdk-root "$PWD"
# this cross-compiles for the example's real SoM (E1M-AEN801) -- it
# needs the Zephyr SDK toolchain pinned in metadata/toolchains.json;
# flash it and open a serial monitor to see output like:
#   [gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED
#   ...
#   [gpio] done
```

`tan bootstrap` is the canonical fresh-clone setup and runs natively on
Linux, macOS, **and Windows** (PowerShell) for the Zephyr-on-M workflow this
walkthrough uses -- it no longer shells out to `bootstrap.sh` (that POSIX-only
refusal was retired; see tan-cli's `bootstrap.windows-unsupported` issue code,
now reserved-but-unused). Only the Yocto/A-class path needs WSL2 (Ubuntu); see
[`docs/cross-platform-setup.md`](cross-platform-setup.md) §4/§5 for the native
Windows walkthrough and the Yocto/WSL2 split. Python Tan performs the
bootstrap phases from the SDK's `metadata/bootstrap.json` on every host: it
creates the Zephyr workspace one level up from `alp-sdk/` (or under
`--workspace <dir>`, relocating the checkout there first if needed), runs
`west update --narrow`, installs the Zephyr Python dependencies and SDK
extras, and reports any missing host prerequisites -- native Windows gets
Windows-specific guidance (7-Zip on PATH for `west sdk install`, the
`arm-zephyr-eabi` toolchain stays a separate manual step everywhere).

`scripts/bootstrap.sh` (and its PowerShell twin `scripts/bootstrap.ps1`
-- see [`docs/cross-platform-setup.md`](cross-platform-setup.md) §4)
does the same job without `tan`, which is what CI and a host that has
no `tan` yet use.  Either is fine; the commands above lead with `tan`
because it is the SDK's documented user command surface (ADR
[0020](adr/0020-sdk-owns-build-execution.md)) and because a reader who
runs one first command everywhere has one thing to debug, not two.

Note the venv is **not** something you activate by hand for a build:
the build plan carries the per-slice `PATH` additions, so `tan build`
resolves `west` itself.  Activate it (`source ../.venv/bin/activate`)
only if you intend to drive `west` directly.

`tan build` validates the example's `board.yaml` with Tan's relocated planner,
generates the build-time config from it, and
delegates to `west build`.  The rest of this document explains *why*
each step is what it is so you can adapt it to your own project.

For a full local verification pass (everything CI runs short of
real-hardware HIL), see [`docs/testing.md`](testing.md):

```bash
bash scripts/test-all.sh
```

## Linux / Yocto path — start here

Everything below targets the **Zephyr / MCU side**: M-class cores
plus `native_sim` on your host.  If you came here for the **Linux
side** of a V2N / V2N-M1 SoM — a kernel + root filesystem for the
Cortex-A55 cluster — that is a separate flow with different
constraints, worth knowing before you allocate an afternoon:

- **The Renesas BSP is license-gated.**  The build consumes the
  RZ/V2N AI SDK BSP Source Code package (`RTK0EF0189F06300SJ`),
  fetched from your own Renesas account.  alp-sdk does not (and
  cannot) redistribute it.
- **Disk + host:** budget **~60 GB free** and a **Linux host or
  WSL2 Ubuntu** — Yocto does not build on native Windows or macOS.
- The bootloader is production-flashed by Alp Lab; you build only
  kernel + rootfs.

Start at [`docs/build-yocto-v2n.md`](build-yocto-v2n.md) for the
V2N-specific BSP / deploy / verification detail, and
[`meta-alp-sdk/README.md`](../meta-alp-sdk/README.md) for the layer
assembly.  The Zephyr sections below still apply to the same SoM's
M33 core — the two paths coexist on one module.

## 1. Prerequisites

The SDK is supported on **macOS**, **Windows**, and **Linux** --
pick whichever you already have.  Tooling versions are identical
across hosts for `native_sim` and day-to-day iteration; real-silicon
Zephyr builds are the one platform-specific exception, and only on
Intel Macs (see below).

**Intel Macs can build and run `native_sim`** (host-toolchain build,
no Zephyr SDK involved) **but not real-silicon Zephyr images or
`tan build`.**  The pinned Zephyr SDK ships no `macos-x86_64` host
build (dropped in `1.0.0`; `macos-aarch64` is not a substitute --
Rosetta translates x86_64 *for* Apple Silicon, not the reverse, and
macOS has no WSL2 fallback).  Real-silicon builds need a Linux host
instead -- see
[`docs/cross-platform-setup.md`](cross-platform-setup.md).

| Tool        | Version          | Notes                                                    |
|-------------|------------------|----------------------------------------------------------|
| Zephyr      | v4.4.1 (stable)  | Pinned by `west.yml`; see [`docs/zephyr-version-policy.md`](zephyr-version-policy.md). |
| Python      | 3.10+ for SDK bootstrap tools; 3.12+ for current Tan source and Zephyr builds | 3.10 is `bootstrap.sh`/`.ps1`'s and `pyproject.toml`'s support **floor**. The Python Tan port and Zephyr v4.4.1's `cmake/modules/python.cmake` require 3.12, so use 3.12+ for the documented end-to-end path; see [`docs/cross-platform-setup.md`](cross-platform-setup.md) §1.1. Dev/CI standardise on the repo-root `.python-version` **pin** (currently 3.12). |
| Python deps | `pyyaml`, `jsonschema`, `imgtool` | All installed by `scripts/bootstrap.sh`; manual install: `pip install pyyaml jsonschema imgtool`. |
| CMake       | 3.20+            | `find_package(Zephyr)` minimum.                          |
| C compiler  | GCC 11+ / Clang 14+ | `native_sim` builds; cross-toolchain for real silicon. |
| west        | 1.2+             | `pip install west` if your distro doesn't ship it.       |

Per-platform install one-liners:

```bash
# macOS (Homebrew)
brew install cmake ninja python git curl
# no `pip3 install west` here -- scripts/bootstrap.sh installs west into
# the workspace venv itself, and Homebrew's Python 3.12 is PEP 668
# externally-managed too, so a system-wide `pip3 install` fails outright

# Linux (Debian / Ubuntu)
sudo apt install -y cmake ninja-build python3 python3-pip python3-venv git curl xz-utils wget
# no `pip3 install west` here -- scripts/bootstrap.sh installs west into
# the workspace venv itself, and on Ubuntu 24.04 running it against the
# system interpreter fails outright (PEP 668 externally-managed-environment)
# xz-utils/wget: scripts/bootstrap.sh hard-refuses without them on Linux --
# GNU tar (what `west sdk install` shells out to) execs a standalone
# /usr/bin/xz to unpack the SDK archive, and the pinned Zephyr SDK's own
# setup.sh hard-checks for `wget` on Linux (issue #949)

# Windows -- PowerShell + Python from Microsoft Store
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
winget install -e --id Python.Python.3.12
pip install west

# Windows -- WSL2 path (use the Linux instructions inside Ubuntu)
wsl --install -d Ubuntu
```

**Verify your setup first.** Before building anything, run Tan's read-only
build/flash readiness preflight. It checks the SDK and project, the resolved
west/Zephyr workspace and pin, the effective Python floor, host prerequisites,
the Zephyr SDK, and programming tools. Each problem includes a fix hint:

```bash
tan doctor                    # human-readable report
tan doctor --format json      # machine-readable
```

It is HW-free (no build and no flash), so it is safe to run anytime. An
unhealthy environment exits 4. `--build` remains accepted for compatibility
with v0.4 callers but no longer changes the checklist. On tan-cli `dev`
(ahead of the tagged `v0.5.1` release the installer above ships -- lands in
`v0.6.0-rc1`+), interactive `--fix` (it needs a TTY on both stdin and
stderr) can run manifest remedies: a remedy that needs no elevation (macOS
`brew`, Windows `winget`) runs for any caller; a `sudo`-prefixed remedy
(Linux `apt`/`dnf`) is refused with the command printed to run by hand for
a non-root caller, and has the literal `sudo ` prefix stripped and the rest
run for a caller who is already root; and without a TTY there are no
repairs.

Every `west build` needs Ninja, so install it before continuing if the report
cannot find it:

```bash
sudo apt-get install -y ninja-build   # Linux
brew install ninja                    # macOS
winget install -e --id Ninja-build.Ninja   # Windows
```

See [`docs/cli.md`](cli.md#tan-doctor) for the
current check contract and [`docs/troubleshooting.md`](troubleshooting.md) for
the failures each check prevents.

For real-silicon builds you'll also need the Zephyr SDK toolchain --
its pinned version/URL/sha256 live in
[`metadata/toolchains.json`](../metadata/toolchains.json), the
single source (see `docs/zephyr-version-policy.md`) -- and a JTAG /
SWD probe matching your board.  Nothing installs it for you; from
your west workspace's top-level directory (the alp-sdk checkout's
parent):

```bash
west sdk install --gnu-toolchains arm-zephyr-eabi --no-hosttools \
    --install-dir "$PWD/zephyr-sdk"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/zephyr-sdk"
```

Select the alp-sdk checkout with `--sdk-root` when you build:

```bash
tan build --project <app-dir> --sdk-root <path-to-this-alp-sdk-checkout>
```

Python Tan resolves SDKs in this order: `--sdk-root`, the project's
`.alp/sdk-path`, a sibling checkout, then `ALP_SDK_ROOT`. `tan sdk install` and
`tan sdk switch` currently refuse with `sdk.not-ported`; do not use the old
persistent-selection commands during the port. See
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md) for the EVK's wiring.

> **Note for Windows users.**  The repo's `.gitattributes` pins
> LF on every source file -- a fresh clone gets identical bytes
> on PowerShell, WSL, and macOS / Linux, so clang-format-diff CI
> doesn't trip on a Windows checkout.

## 2. Two consumer paths

alp-sdk is standalone — nothing in this walkthrough requires
alp-studio.  Both consumer paths below are first-class; pick
whichever fits.  ADR
[0001](adr/0001-wrapper-on-top-of-zephyr.md) explains the
rationale.

### 2.1 Standalone (this walkthrough)

You write Zephyr / Yocto / bare-metal app code directly against
`<alp/...>` headers.  Pick instance IDs from the pinout namespace
for your form factor: `<alp/e1m_pinout.h>` exposes E1M IDs such as
`ALP_E1M_I2C0` / `ALP_E1M_PWM3`; `<alp/e1m_x_pinout.h>` exposes
the parallel E1M-X IDs such as `ALP_E1M_X_I2C0` /
`ALP_E1M_X_PWM3`.  Your app stays portable within that form factor
and SoM family.  The rest of this document covers this path.

### 2.2 alp-studio codegen (optional, on top of alp-sdk)

alp-studio is a consumer that sits on top of alp-sdk: its pin
allocator reads the SoM preset + `pad_routes:` from this repo's
`metadata/e1m_modules/<SKU>.yaml`, ingests block manifests, and
emits the same `<alp/...>` calls you'd write by hand.  Switching
to studio at any point is non-destructive — your standalone app
keeps working alongside the studio-generated code.  See
`alplabai/alp-studio` (not a public GitHub repo).

## 3. Set up a Zephyr workspace

The SDK is a Zephyr **module** — your build pulls Zephyr,
modules, and the SDK in one go via `west`.

```bash
mkdir alp-workspace && cd alp-workspace
west init -m https://github.com/alplabai/alp-sdk
west update --narrow -o=--depth=1
west zephyr-export
```

After this:

- `alp-workspace/zephyr/`    Zephyr v4.4.1 (pinned via the SDK's `west.yml`).
- `alp-workspace/modules/`   Zephyr's standard modules (HAL, libs).
- `alp-workspace/alp-sdk/`   This repo, mounted as a Zephyr module.

`west update --narrow -o=--depth=1` keeps the clone shallow —
saves ~30 GB of unrelated git history.

The steps above get you Zephyr itself, not a cross toolchain — every
example below targets a real SoM, so install the Zephyr SDK's
`arm-zephyr-eabi` too (one-time, still from `alp-workspace`):

```bash
west sdk install --gnu-toolchains arm-zephyr-eabi --no-hosttools \
    --install-dir "$PWD/zephyr-sdk"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/zephyr-sdk"
```

Importing alp-sdk via `west init -m` also surfaces the SDK's
surviving west-extension commands — `west alp-migrate` (board.yaml
schema migration), `west alp-lock` (dependency lockfile), `west
alp-quality` (the quality-task registry), and `west alp-emit` (the
read-only artefact inspector), all registered via
`scripts/west-commands.yml`.  Building is not one of them: the rest
of this walkthrough uses the standalone `tan` CLI
([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)), which
plans from alp-sdk's metadata in process and drives `west build` itself.
alp-sdk's `alp_orchestrate --emit build-plan` remains the reference/parity
producer. See [`docs/cli.md`](cli.md) for the full Tan verb reference.

## 4. First build: the GPIO example

Every example in `examples/` carries a **`board.yaml`** — the
single declarative file the loader compiles into Kconfig
fragments, DTS overlays, and the build-time hw_info header.  `tan
build` does the pre-flight + delegates to `west build`:

```bash
cd alp-workspace
tan build --project alp-sdk/examples/peripheral-io/gpio-button-led \
    --sdk-root "$PWD/alp-sdk"
```

`--sdk-root` selects the checkout for this invocation. A committed
`.alp/sdk-path` can provide the same project-local selection when you control
the application repository.

What this does:

- `--project alp-sdk/examples/peripheral-io/gpio-button-led` — the
  application directory.  Each example under `examples/` ships a
  `board.yaml` + an empty `prj.conf` + a CMakeLists.txt that invokes
  the loader at configure time.  See
  [`docs/board-config-schema.md`](board-config-schema.md) for the
  schema.
- The target comes entirely from that `board.yaml` — there is no
  `--board` selector.  This example's `board.yaml` targets a real
  SoM (`E1M-AEN801`), so this build cross-compiles and needs the
  Zephyr SDK toolchain pinned in
  [`metadata/toolchains.json`](../metadata/toolchains.json).
  `board.yaml`'s `som.sku` is pattern-locked to real SoM SKUs
  (`metadata/schemas/board.schema.json`) — `native_sim` is not a
  `board.yaml` target; it's reached through twister /
  `testcase.yaml`'s `platform_allow`, a separate mechanism.

`tan build` walks four steps under the hood, driven by Tan's relocated
in-process planner. The SDK's `alp_orchestrate --emit build-plan` path is kept
as its reference/parity producer:

1. **Validates** the app's `board.yaml` (schema + SoM SKU preset +
   board preset + `hw_rev` / SDK-version compatibility window) — run
   `tan validate` first for the full check, including the advisory
   `peripherals:` vs SoC caps cross-check; `tan build` plans in-process
   and never spawns the SDK validator.
2. **Materialises** every generated artefact the plan carries,
   including the build-time hw_info header at
   `<build>/generated/alp_hw_info_build.h` so apps that include
   it pick up the `ALP_HW_BUILD_*` macros.
3. **Sets** `EXTRA_ZEPHYR_MODULES` + `ALP_SDK_ROOT` (plus any
   `envAppendPath` entries the plan carries) so the application's
   CMakeLists.txt resolves the SDK without per-customer overrides.
4. **Delegates** to `west build`, then — for a `native_sim` target —
   runs the produced binary (`build/native_sim/zephyr/zephyr.exe`)
   directly and streams its stdout; no separate `tan run` step is
   needed.

This example targets real silicon, so `tan build` above only cross-compiles
it (step 4's "runs the produced binary" applies to a `native_sim` target,
not this one) — flash it and open a serial monitor to see illustrative
output like this (exact ordering/timing may vary):

```
*** Booting Zephyr OS build v4.4.1 ***
[gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED
[gpio] led=0 status=0
[gpio] led=1 status=0
[gpio] led=0 status=0
[gpio] led=1 status=0
[gpio] is_pressed -> status=0 pressed=1
[gpio] done
```

`status=0` means `ALP_OK`.  `pressed=1` is gpio_emul's default
"input is low" report; on a real button it depends on the
press state.

## 5. Read the example

Open `alp-sdk/examples/peripheral-io/gpio-button-led/src/main.c`.  Every
example app is annotated as teaching material — the comments
spell out:

- What each `alp_*_open` config field means and what the alternative
  values would do.
- Why specific values were chosen (timing, geometry, mode).
- What `alp_last_error()` returns on each failure mode.
- Lifecycle expectations of `*_close`.

Read the `gpio-button-led` example through, then look at
`<alp/blocks/button_led.h>` to see what API the example is
calling into.  The button_led helper (`alp_button_led_*`) is
SDK-level; the underlying primitives (`alp_gpio_*`) are in
`<alp/peripheral.h>`.

## 6. Run more examples

Every wrapped peripheral has a corresponding example:

```bash
for ex in peripheral-io/pwm-led-fade peripheral-io/adc-voltmeter \
          peripheral-io/i2c-scanner peripheral-io/spi-loopback \
          peripheral-io/uart-echo peripheral-io/uart-rx-ringbuf \
          peripheral-io/can-loopback peripheral-io/qenc-readout \
          power-timing/counter-alarm power-timing/rtc-clock \
          power-timing/wdt-feed audio/i2s-tone; do
    # --sdk-root applies to this invocation, so the loop needs no persistent
    # per-project pointer.
    tan build --sdk-root "$PWD/alp-sdk" --project alp-sdk/examples/$ex
done
```

On `native_sim` most peripherals don't have emul controllers
(only I²C / SPI / GPIO / UART do).  The examples that target
unwrapped peripherals exit after printing the
`alp_last_error()` diagnostic — that's expected and proves the
wrapper plumbing compiles + links cleanly.

## 6.5. Pull in a curated third-party library

Need a GUI, DSP, or serialization library?  Add one line to your
project's `board.yaml` — the top-level `libraries:` key (ADR 0018):

```yaml
som:
  sku: E1M-AEN801
libraries: [lvgl, cmsis-dsp]   # curated third-party libraries
cores:
  m55_hp:
    app: ./src
```

Each name resolves to a manifest under
[`metadata/libraries/`](../metadata/libraries/); the loader emits the
right wiring per OS (Zephyr `CONFIG_LVGL=y` in `alp.conf`, Yocto
`IMAGE_INSTALL` for the A-cores, …) and refuses a library the target
can't run, naming the failing constraint.  Check what's selected and
whether it's compatible — run this from *your project's* directory
(the one containing the `board.yaml` above), not from the SDK
checkout: the `libraries` line only appears when `doctor` is run
where it can find that `board.yaml`, and `PYTHONPATH` has to reach
the checkout's `scripts/` from wherever that project lives:

```bash
cd <your-project-dir>   # contains the board.yaml you just edited
PYTHONPATH=<path-to-alp-sdk>/scripts python3 -m alp_cli doctor   # a "libraries" line reports tier + licence + fit
```

`doctor`'s library check only handles the bare-string `libraries: [name,
...]` form shown above today; the dict form (`- name: lvgl` / `cores:
[...]`, also schema-valid and what several in-tree examples use) currently
makes it abort with an uncaught error instead of reporting — a `doctor.py`
fix, not a docs one, tracked separately.

The curated set today: `lvgl`, `cmsis-dsp`, `cmsis-nn`, `nanopb`,
`zcbor`, `modbus` (all Tier A), plus a growing Tier B (recipe-only) set
including `cmsis-stream`, `cmsis-cv`, and `arm-2d`.  See
[`metadata/libraries/README.md`](../metadata/libraries/) for
the full list and how to add one.

## 7. Targeting real silicon

Real board files ship in-tree under [`zephyr/boards/alp/`](../zephyr/boards/alp/)
(exposed via `zephyr/module.yml`'s `board_root: zephyr` — no external
repo to wait on).  There is no `--board` flag: the target comes from
the project's `board.yaml` `som.sku` field, which `tan build` resolves
to the qualified Zephyr board string for you.  Point `board.yaml` at
E1M-AEN801 (Alif Ensemble E8, M55-HE) —

```yaml
som:
  sku: E1M-AEN801
```

— then build and flash the same way:

```bash
tan build --project alp-sdk/examples/peripheral-io/gpio-button-led
tan flash alp-sdk/examples/peripheral-io/gpio-button-led
```

or point it at E1M-V2N101 (Renesas RZ/V2N, Cortex-M33 system manager)
instead:

```yaml
som:
  sku: E1M-V2N101
```

```bash
tan build --project alp-sdk/examples/peripheral-io/gpio-button-led
tan flash alp-sdk/examples/peripheral-io/gpio-button-led
```

Once the board is running, `tan monitor --port <port>` opens its
serial console (run it portless to list the host's serial ports;
`--baud` overrides the 115200 default) — `tan monitor` never builds
anything, so it's unaffected by the executor split.  See
[`docs/cli.md`](cli.md) for `tan`'s full verb reference.

Each example's `boards/` directory has an overlay that maps
the example's `alp,pin-array` slots to specific EVK pins.  The
overlay applies automatically when you build for the matching
board.

For SoMs without an EVK board file yet, write your own:

1. Create a board file in-tree under
   [`zephyr/boards/alp/`](../zephyr/boards/alp/) (or a private
   board layer) — `zephyr/module.yml`'s `board_root: zephyr`
   exposes it to Zephyr's board scanner without any external repo.
2. Define the `alp,pin-array` node + `alp-i2cN` / `alp-spiN` /
   `alp-pwmN` / etc. aliases in your board's DTS.
3. Add a `boards/<your_board>.overlay` to the example with any
   board-specific pin remapping.

`docs/porting-new-som.md` covers the full porting checklist.

## 8. Vendor licences when integrating against real silicon

The SDK itself ships under Apache-2.0 (see `LICENSE`).  Once you
target a specific silicon backend, you also pull source from the
vendor's public SDK -- and each vendor's terms apply to that
source.  All four vendor SDKs in the v1.0 matrix are **publicly
source-visible on GitHub** with steady release cadences, but the
licence flavour differs.  Customer projects should be clear about
which licence applies to which subtree before shipping.

| Vendor       | SDK repo                                                                                | Latest tag        | Licence shape                                                                                                                                                                                                                                                            |
|--------------|------------------------------------------------------------------------------------------|-------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Alif**     | [`alifsemi/sdk-alif`](https://github.com/alifsemi/sdk-alif) + 58 sibling repos           | v2.3.0-rc1        | Two-bucket: forks of upstream OSS keep upstream licensing (`zephyr_alif` / `hal_alif` / `cmsis_alif` / `mcuboot_alif` / `matter_alif` Apache-2.0; `meta-alif*` Yocto layers MIT).  Differentiating drivers (`sdk-alif`, `alif_dave2d-driver`, ML eval kit, ISP helpers) ride a **vendor-specific "Alif Semiconductor Software License Agreement"** -- source-visible but with Alif's terms. |
| **Renesas**  | [`renesas/rzv-fsp`](https://github.com/renesas/rzv-fsp)                                  | v3.1.0 (Mar 2025) | **BSD-3-Clause** for the MPU BSP / Board BSP / HAL / generic middleware (the parts the SDK consumes).  `rzv2n_evk` board support included.  A handful of pre-compiled middleware modules (e.g. `rm_zmod4xxx`) ship under Renesas's own software-licence agreement -- per-component table in the FSP repo's `LICENSE.md`.                                                                          |
| **NXP**      | [`nxp-mcuxpresso/mcuxsdk-manifests`](https://github.com/nxp-mcuxpresso/mcuxsdk-manifests) | v26.03.00          | **NXP-specific licence**: `LA_OPT_Online Code Hosting NXP_Software_License v1.4` (May 2025).  Acceptance implied by clone / install / use.  Source-visible, not Apache / BSD.  Yocto-side via `meta-imx` is a separate release cycle.                                                                                                                                                                  |
| **DEEPX**    | [`DEEPX-AI`](https://github.com/DEEPX-AI) (30+ repos)                                    | dx_rt 2026-05-11   | Two-bucket: firmware images (`dx_fw`) Apache-2.0; model zoo (`dx-modelzoo`) MIT.  Runtime (`dx_rt`), app templates (`dx_app`), Linux PCIe driver (`dx_rt_npu_linux_driver`), Windows runtime (`dx_rt_windows`) **source-visible but customer-only** -- restricted to "customers supplied with DEEPX NPU".  Yocto recipes (`meta-deepx-m1`) have no LICENSE file -- ask DEEPX before redistributing. |

### What this means for *your* project

- **You can clone, study, and develop against** every repo above
  without signing anything -- they're all on public GitHub.
- **For shipping production firmware / Yocto images** that
  redistribute vendor source, check each component's licence
  text before stripping or relicensing.  The SDK's own
  Apache-2.0 sits cleanly on top of all four; what you have to
  manage is what *you* redistribute downstream.
- **`chips/deepx_dxm1/`** is our own Apache-2.0 thin host
  driver; it does *not* redistribute DEEPX runtime code.  When
  you flip `CONFIG_ALP_SDK_CHIP_DEEPX_DXM1=y` you become the
  party who fetches `dx_rt` from the DEEPX repo (as a DEEPX NPU
  customer) -- the SDK only links against headers.
- **The DRP-AI compiler toolchain (Renesas) + DEEPX
  meta-deepx-m1 LICENSE clarification** are still open
  vendor-side -- see [`docs/vendor-partnerships.md`](vendor-partnerships.md)
  for the current state.

The SDK's CI consumes only the permissively-licensed (Apache /
BSD / MIT) subtrees of each vendor SDK so the public build is
unencumbered.  Customer integrations that need the
vendor-licensed bits add them to their own west.yml / Yocto
recipes, not to ours.

### How each vendor SDK reaches your Zephyr build

The vendor SDKs land in your workspace through three different
paths.  Critically, **Alif is the exception** -- it does NOT
ship as a `hal_*` module inside Zephyr's manifest, so the
default `west update` skips it.

| Vendor   | Zephyr v4.4 import path                            | What you need to do                                                                                     |
|----------|----------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| **Renesas (RZ/V)** | `hal_renesas` (in Zephyr's own west.yml)   | Nothing extra.  Our `name-allowlist` lets Zephyr import it; `drivers/rz/fsp/src/rzv/bsp/mcu/rzv2n/` is what the V2N + V2N-M1 paths consume. |
| **NXP (i.MX 9x)**  | `hal_nxp` (in Zephyr's own west.yml)       | Nothing extra.  `mcux/mcux-sdk-ng/devices/i.MX/i.MX93/` covers MIMX9301..9352 (E1M-NX9101 = MIMX9352).   |
| **Alif (Ensemble)** | `hal_alif` (in our west.yml, from Alif's own GitHub) + upstream Zephyr `boards/alif/` | **Simpler than v3.7.**  HAL drivers come from `alifsemi/hal_alif v2.3.0` (Apache-2.0) which we pin as a top-level project — fetched on every `west update`.  Upstream Zephyr v4.4 also ships the stock Alif Ensemble board files under `boards/alif/` (`ensemble_e8_dk`, `ensemble_e1c_dk`, `balletto_b1_dk`) -- those target Alif's own EVKs, not the E1M board.  The AEN-specific board files (`alp_e1m_aen801_m55_he`, `alp_e1m_aen801_m55_hp`, `alp_e1m_aen401_m55_hp`, `alp_e1m_aen601_m55_hp`) ship in-tree at [`zephyr/boards/alp/`](../zephyr/boards/alp/) -- no separate overlay or repo needed.  Two Alif drivers (`alif_dave2d-driver`, `alif_image-processing-lib`) are vendor-licensed and sit in the `vendor-sdks` opt-in group; enable when you need DAVE2D / Helium image kernels.  See `docs/vendor-partnerships.md` §Alif for the migration history. |
| **DEEPX (DX-M1)**  | Out of Zephyr scope (Linux-side runtime).  | The on-device NPU runs from a Linux PCIe driver, not a Zephyr backend.  `chips/deepx_dxm1/` is the **host-side** Zephyr code that brings up the M1 from the Renesas A55 cluster; `dx_rt` itself rides on Linux/Yocto.  See `examples/v2n/v2n-m1-deepx-inference/` and the customer-side integration notes in `docs/vendor-partnerships.md` §DEEPX. |

### Bare-metal / non-Zephyr customers

If you're not using Zephyr -- a bare-metal MCU build, a Yocto
image that talks directly to silicon, or a custom RTOS -- the
`vendor-sdks` group pins the bare-metal-side vendor source
trees + the Alif vendor-licensed drivers behind one
opt-in:

```bash
west update --group-filter +vendor-sdks
ls modules/                       # hal/alif/ (always)
ls modules/vendors/                # rzv-fsp/ mcuxsdk-manifests/ (group-on)
ls modules/drivers/                # dave2d/ (group-on)
ls modules/lib/                    # aipl/ (group-on)
```

For Renesas + NXP the `vendor-sdks` pins (`rzv-fsp`,
`mcuxsdk-manifests`) duplicate `hal_renesas` / `hal_nxp` --
intentional, so bare-metal customers don't have to dig through
Zephyr's module organisation.  The Alif vendor-licensed pieces
(`alif_dave2d-driver`, `alif_image-processing-lib`) are
distinct from the Apache-2.0 `hal_alif` -- they're only
fetched when a customer opts in to the `vendor-sdks` group.

## Reproducing a build with alp.lock

`west alp-lock` writes `alp.lock` — a deterministic, public-safe record of the
workspace's SDK version, west project pins, curated library versions, Python
requirements, and metadata digests. Commit it. `west alp-lock --check` (run in
CI) fails with a field-level diagnostic when any locked input drifts, so an old
release can be rebuilt against its exact declared inputs. It contains no local
paths or credentials.

`sdk.version` and the west pins lock the SDK identity you build against. The
lock no longer records an `sdk.revision` (#1615) — a git HEAD written into a
file that is then committed is stale the moment it lands — and every remaining
input is file-derived, so regenerating over an unchanged tree reproduces the
document byte for byte. A lock generated before that change still verifies
clean. For "which SDK commit produced *this artifact*", use the build receipt
(`scripts/build_receipt.py`), whose `source.sdkRevision` and `source.sdkDirty`
are resolved against a real build rather than baked into a committed file —
worth reaching for between release tags, where many commits share one
`sdk.version` and the lock's digests cover `metadata/**` but not `src/` or
`include/`. The lock does not yet pin resolved commit SHAs or toolchain
container identities (tracked follow-ups).

Because `alp.lock` hashes `metadata/**` and pins the west/library/Python inputs,
**re-run `west alp-lock` and commit the updated `alp.lock` in the same PR**
whenever you touch `west.yml`, `metadata/**`, `scripts/requirements.txt`, or
`scripts/alp_cli/__init__.py` — otherwise the `alp.lock in sync` CI check reds.

## 9. SoC capability validation

The SoC choice flows from `board.yaml`'s `som.sku` field
automatically (board.yaml, current since v0.6) — the loader
resolves the MPN to the silicon ref (`alif:ensemble:e8` for
`E1M-AEN801`) and emits the matching
`CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y` line, so you never set it by
hand.  The validator also cross-checks every entry in
`peripherals:` against the SoC's `metadata/socs/<vendor>/<family>/<part>.json`
caps -- a board.yaml asking for `i2s` on a SoC that doesn't route
I²S is reported by `tan validate` as an `ALP-B010` **warning**
(`scripts/validate_board_yaml.py` still exits 0; the check is advisory
because SoC peripheral metadata is incomplete for some parts and some
peripherals are board-side). `tan build` does not re-run this
cross-check at all.

At runtime, the documented caps drive the per-`*_open` validation:
e.g. `alp_adc_open` with `resolution_bits = 16` on a 12-bit SoC
returns NULL with `alp_last_error() == ALP_ERR_OUT_OF_RANGE`.  See
ADR [0002](adr/0002-error-mechanism.md) for the diagnostic contract.

## 10. Editing in VS Code

Two complementary surfaces:

**The SDK's `.vscode/` config** (`extensions`, `settings`, `tasks`,
`c_cpp_properties`) is set up for Zephyr-module + plain-CMake
development.  See the "Using with VS Code" section in
[`README.md`](../README.md).

**The `alplabai.alp-sdk` extension** ([source: `alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode))
adds schema-aware `board.yaml` editing (autocomplete on SKUs,
boards, libraries; inline diagnostics from `validate_board_yaml.py`
in the Problems panel), a GUI configurator panel with dropdowns
for supported SoM presets + boards, `tan` wrappers (build / flash /
run native_sim), per-OS dependency bootstrap, and a one-keypress
*Alp: Generate all* command for the four emit modes (`zephyr-conf`,
`dts-overlay`, `cmake-args`, `yocto-conf`).  Install from the
[VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=alplabai.alp-sdk)
for normal use.  To build from source instead (a separate repo --
there is no `vscode/` directory in alp-sdk itself):

```bash
git clone https://github.com/alplabai/alp-sdk-vscode && cd alp-sdk-vscode
pnpm install && pnpm run package
code --install-extension alp-sdk-*.vsix
```

Note: this is a **pnpm workspace, not npm** -- don't run `npm install` /
`npm run package` against it.

Key tasks (Command Palette → **Tasks: Run Task**):

- `validate · metadata` — runs `validate_metadata.py`.
- `regen · soc_caps.h` / `regen · ABI snapshot` — regenerate
  generated artefacts after touching `metadata/` or `include/`.
- `twister · all` — runs the full ztest + example suite under
  `native_sim`.
- `west build · edgeai-vision-aen` / `iot-connected-camera` —
  builds the end-to-end reference apps.

## 11. Where to go next

- **[`docs/board-config-schema.md`](board-config-schema.md)** -- the
  authoritative `board.yaml` schema reference.  Start here when
  you're ready to write your own app's `board.yaml`.
- **[`docs/board-config-emit.md`](board-config-emit.md)** -- the
  recipe table for every loader emit mode (`zephyr-conf`,
  `cmake-args`, `yocto-conf`, `dts-overlay`, `hw-info-h`,
  `west-libraries`).
- **Per-peripheral examples**: [`examples/`](../examples/README.md)
  -- 11 minimal apps, one per `<alp/*.h>` class, each driven by a
  matching `board.yaml`.
- **End-to-end reference apps**:
  [`examples/aen/edgeai-vision-aen/`](../examples/aen/edgeai-vision-aen/)
  (camera → Ethos-U inference → display) and
  [`examples/connectivity/iot-connected-camera/`](../examples/connectivity/iot-connected-camera/)
  (camera → DRP-AI → MQTT publish).  Both use the same board.yaml
  workflow at a larger scale.
- **Hardware identification + production-test**:
  [`<alp/hw_info.h>`](../include/alp/hw_info.h) for the runtime
  on-module EEPROM-manifest API; `scripts/program_eeprom.py`
  for the factory programmer.
- **Recommended third-party libraries** that pair with the SDK
  (CMSIS-DSP, ETL, fmt, nlohmann_json, doctest, LittleFS, LVGL,
  MbedTLS): [`docs/recommended-libraries.md`](recommended-libraries.md).
- **CC3501E Wi-Fi/BLE coprocessor bridge** (E1M-AEN family):
  [`docs/cc3501e-bridge.md`](cc3501e-bridge.md).
- **Zephyr-version policy** -- when LTS bumps drive new alp-sdk
  releases: [`docs/zephyr-version-policy.md`](zephyr-version-policy.md).
- **Architecture overview**: [`docs/architecture.md`](architecture.md)
- **Architecture decision records**: [`docs/adr/`](adr/)
- **Hardware specs**: [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec)
- **Per-version roadmap**: [`VERSIONS.md`](../VERSIONS.md)
- **Contributor guide**: [`CONTRIBUTING.md`](../CONTRIBUTING.md)
- **Testing coverage map**: [`docs/testing.md`](testing.md)
- **Verification ledger** (⏳/🟡/✅): [`docs/test-plan.md`](test-plan.md)
- **Secure boot chain + key lifecycle**: [`docs/secure-boot.md`](secure-boot.md)
- **OTA strategy** (Yocto Mender + AEN-Zephyr pending decision): [`docs/ota.md`](ota.md)
