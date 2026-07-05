# Getting started with the Alp SDK

This walkthrough takes you from "git clone" to a working
`gpio-button-led` build under `native_sim`, then onto real
silicon.  No `alp-studio` required — the SDK supports
hand-written firmware as a first-class consumer.

> **Rendered version:** the full SDK documentation site lives at
> [**docs.alplab.ai/sdk/introduction**](https://docs.alplab.ai/sdk/introduction).
> This in-repo markdown is the source of truth; the site mirrors
> it with cross-version navigation + search.  Stuck on something?
> Ask on [**community.alplab.ai**](https://community.alplab.ai/).

> **Two front ends: `alp` CLI vs `west`.**  The SDK ships two
> equivalent entry points, and both consume the same `board.yaml`:
>
> - **`alp` CLI** — the single-image quick path.  `pip install -e .`
>   once per clone, then `alp init` scaffolds a project and `alp run`
>   builds it for `native_sim` and prints its stdout straight
>   through.  This is the headline
>   [README Quickstart](../README.md#quickstart) — if you just want
>   a hello-world running in two minutes, start there.  The full
>   verb reference (`build` / `flash` / `emit` / `doctor` /
>   `monitor` / `new-som` / …) lives in [`docs/cli.md`](cli.md).
> - **`west alp-build`** — the multi-core / heterogeneous path this
>   walkthrough uses.  It fans a `board.yaml` out into per-core
>   build slices, runs the full pre-flight (schema validation, SoC
>   caps, hw_info header) and delegates to `west build`.
>
> Nothing is lost switching between them: `alp run` uses the same
> loader and validator under the hood.  Pick `alp` for a first
> taste, `west alp-build` once your project spans more than one
> core or OS.

If you'd rather skim, the fastest path is:

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk
bash scripts/bootstrap.sh                            # one-time: west + Python + apt hints
export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
west alp-build -b native_sim/native/64 examples/peripheral-io/gpio-button-led
west build -d build -t run
# expect: [gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED
#          ...
#          [gpio] done
```

`scripts/bootstrap.sh` is the canonical fresh-clone setup -- it
creates the Zephyr workspace one level up from `alp-sdk/`, runs
`west update --narrow`, installs the Zephyr Python deps + the
SDK's extras (`jsonschema`, `imgtool`), and prints OS-specific
`apt` / `brew` commands for the optional native libraries the
Yocto-side backends need.  Windows-native users run the PowerShell
twin, `scripts/bootstrap.ps1`, instead (see
[`docs/cross-platform-setup.md`](cross-platform-setup.md) §4).

`west alp-build` validates the example's `board.yaml`, generates
the build-time config from it, and delegates to `west build`.
The rest of this document explains *why* each step is what it is
so you can adapt it to your own project.

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

The SDK is supported equally on **macOS**, **Windows**, and
**Linux** -- pick whichever you already have.  Tooling versions
are identical across hosts; the only platform-specific bit is how
you install them.

| Tool        | Version          | Notes                                                    |
|-------------|------------------|----------------------------------------------------------|
| Zephyr      | v4.4.0 (stable)  | Pinned by `west.yml`; see [`docs/zephyr-version-policy.md`](zephyr-version-policy.md). |
| Python      | 3.10+ (dev/CI pin: 3.12) | 3.10 is the support **floor** (`pyproject.toml` `requires-python`); dev/CI standardise on the **pin** in the repo-root `.python-version` file. Match the pin to reproduce CI exactly -- `alp doctor` warns on a mismatch. |
| Python deps | `pyyaml`, `jsonschema`, `imgtool` | All installed by `scripts/bootstrap.sh`; manual install: `pip install pyyaml jsonschema imgtool`. |
| CMake       | 3.20+            | `find_package(Zephyr)` minimum.                          |
| C compiler  | GCC 11+ / Clang 14+ | `native_sim` builds; cross-toolchain for real silicon. |
| west        | 1.2+             | `pip install west` if your distro doesn't ship it.       |

Per-platform install one-liners:

```bash
# macOS (Homebrew)
brew install cmake ninja python git
pip3 install west

# Linux (Debian / Ubuntu)
sudo apt install -y cmake ninja-build python3 python3-pip git
pip3 install west

# Windows -- PowerShell + Python from Microsoft Store
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
winget install -e --id Python.Python.3.12
pip install west

# Windows -- WSL2 path (use the Linux instructions inside Ubuntu)
wsl --install -d Ubuntu
```

**Verify your setup first.**  Before building anything, run the
read-only preflight -- it checks every tool above (plus the Zephyr
pin, the `.west` workspace and the workspace venv) and prints a
`[PASS]`/`[WARN]`/`[FAIL]` line with a fix hint for each:

```bash
alp doctor              # human-readable report; exit 1 on any FAIL
alp doctor --strict     # also fail on WARN (handy in CI)
alp doctor --json       # machine-readable (used by the VS Code extension)
```

It is HW-free (no build, no board, no flash), so it is safe to run
anytime.  Resolve every `[FAIL]` before continuing; `[WARN]` lines
are for optional / real-silicon-only tooling (Zephyr SDK, hal_alif).

For real-silicon builds you'll also need the Zephyr SDK
(`zephyr-sdk-1.0.1` matches the v0.6 Zephyr v4.4 pin — see
`docs/zephyr-version-policy.md`) and a JTAG / SWD probe matching
your board.  See [`docs/boards/e1m-evk.md`](boards/e1m-evk.md) for
the EVK's wiring.

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
`<alp/...>` headers.  Pick instance IDs by hand from
`<alp/e1m_pinout.h>` (`ALP_E1M_I2C0`, `ALP_E1M_PWM3`, …).  Your
app stays portable across every E1M-conformant SoM.  The rest
of this document covers this path.

### 2.2 alp-studio codegen (optional, on top of alp-sdk)

alp-studio is a consumer that sits on top of alp-sdk: its pin
allocator reads the SoM preset + `pad_routes:` from this repo's
`metadata/e1m_modules/<SKU>.yaml`, ingests block manifests, and
emits the same `<alp/...>` calls you'd write by hand.  Switching
to studio at any point is non-destructive — your standalone app
keeps working alongside the studio-generated code.  See
[`alplabai/alp-studio`](https://github.com/alplabai/alp-studio).

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

- `alp-workspace/zephyr/`    Zephyr v4.4.0 (pinned via the SDK's `west.yml`).
- `alp-workspace/modules/`   Zephyr's standard modules (HAL, libs).
- `alp-workspace/alp-sdk/`   This repo, mounted as a Zephyr module.

`west update --narrow -o=--depth=1` keeps the clone shallow —
saves ~30 GB of unrelated git history.

Importing alp-sdk via `west init -m` also surfaces the SDK's
west-extension commands — `west alp-build`
(`scripts/west_commands/alp_build.py`) plus its siblings
`alp-image` / `alp-flash` / `alp-clean` / `alp-emit` / `alp-size` /
`alp-renode`, all registered via `scripts/west-commands.yml` — that
the rest of this walkthrough uses.  Every one of them is equally
reachable through the `alp` CLI (`alp build`, `alp flash`, `alp size`,
…) documented in [`docs/cli.md`](cli.md); the two front doors drive
the same pipeline.

## 4. First build: the GPIO example

Every example in `examples/` carries a **`board.yaml`** — the
single declarative file the loader compiles into Kconfig
fragments, DTS overlays, and the build-time hw_info header.  The
`west alp-build` wrapper does the pre-flight + delegates to
`west build`:

```bash
cd alp-workspace
west alp-build -b native_sim/native/64 alp-sdk/examples/peripheral-io/gpio-button-led
```

What the flags mean:

- `-b native_sim/native/64` — Zephyr's POSIX simulator on a 64-bit
  host.  No silicon needed; runs as a native process on Linux /
  macOS / WSL.  Upstream Zephyr's `native_sim` is Linux/macOS only;
  on native Windows there is no `native_sim` target — run it through
  WSL2 (Ubuntu).
- `alp-sdk/examples/peripheral-io/gpio-button-led` — the application directory.
  Each example under `examples/` ships a `board.yaml` + an empty
  `prj.conf` + a CMakeLists.txt that invokes the loader at
  configure time.  See [`docs/board-config.md`](board-config.md)
  for the schema.

`west alp-build` walks four steps under the hood:

1. **Validates** the app's `board.yaml` via `validate_board_yaml.py`
   (schema + SoM SKU preset + board preset + `hw_rev` /
   SDK-version compatibility window + `peripherals:` vs SoC caps).
2. **Pre-generates** the build-time hw_info header at
   `<build>/generated/alp_hw_info_build.h` so apps that include
   it pick up the `ALP_HW_BUILD_*` macros.
3. **Sets** `EXTRA_ZEPHYR_MODULES` + `ALP_SDK_ROOT` so the
   application's CMakeLists.txt resolves the SDK without
   per-customer overrides.
4. **Delegates** to `west build`.  Any flags after the recognised
   ones pass through verbatim (e.g. `-d <build-dir>`).

Run it:

```bash
west build -d build -t run
```

Expected output:

```
*** Booting Zephyr OS build v4.4.0 ***
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
    west build -b native_sim/native/64 alp-sdk/examples/$ex \
        -d build/$(basename $ex)
    west build -d build/$(basename $ex) -t run
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
  sku: E1M-AEN701
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
whether it's compatible:

```bash
alp doctor            # a "libraries" line reports tier + licence + fit
```

The curated set today: `lvgl`, `cmsis-dsp`, `cmsis-nn`, `nanopb`,
`zcbor` (all Tier A).  See
[`metadata/libraries/README.md`](../metadata/libraries/README.md) for
the full list and how to add one.

## 7. Targeting real silicon

When `alplabai/alp-zephyr-modules` publishes the EVK board
files (`alp_e1m_evk_aen` for the AEN family, `alp_e1m_evk_v2n`
for V2N), build with that as the `-b` target:

```bash
west build -b alp_e1m_evk_aen alp-sdk/examples/peripheral-io/gpio-button-led
west flash
```

The `alp` CLI equivalents are `alp build --board <name>` and
`alp flash`; once the board is running, `alp monitor --port <port>`
opens its serial console (run it portless to list the host's serial
ports; `--baud` overrides the 115200 default).  See
[`docs/cli.md`](cli.md).

Each example's `boards/` directory has an overlay that maps
the example's `alp,pin-array` slots to specific EVK pins.  The
overlay applies automatically when you build for the matching
board.

For SoMs without an EVK board file yet, write your own:

1. Create a board file under `alp-zephyr-modules` (or a private
   board layer).
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
| **Alif (Ensemble)** | `hal_alif` (in our west.yml, from Alif's own GitHub) + upstream Zephyr `boards/alif/` | **Simpler than v3.7.**  HAL drivers come from `alifsemi/hal_alif v2.2.0` (Apache-2.0) which we pin as a top-level project — fetched on every `west update`.  Upstream Zephyr v4.4 also ships the stock Alif Ensemble board files under `boards/alif/` (`ensemble_e8_dk`, `ensemble_e1c_dk`, `balletto_b1_dk`).  Customers wanting AEN-specific boards still need their own board overlay (write one under `alplabai/alp-zephyr-modules`) -- the upstream files target Alif's own EVKs, not the E1M board.  Two Alif drivers (`alif_dave2d-driver`, `alif_image-processing-lib`) are vendor-licensed and sit in the `vendor-sdks` opt-in group; enable when you need DAVE2D / Helium image kernels.  See `docs/vendor-partnerships.md` §Alif for the migration history. |
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

## 9. SoC capability validation

The SoC choice flows from `board.yaml`'s `som.sku` field
automatically (board.yaml, current since v0.6) — the loader
resolves the MPN to the silicon ref (`alif:ensemble:e7` for
`E1M-AEN701`) and emits the matching
`CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y` line, so you never set it by
hand.  The validator also cross-checks every entry in
`peripherals:` against the SoC's `metadata/socs/<vendor>/<family>/<part>.json`
caps -- a board.yaml asking for `i2s` on a SoC that doesn't route
I²S fails at `west alp-build` time with exit code 3, before any
compile work.

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
for supported SoM presets + boards, west wrappers (build / flash /
run native_sim), per-OS dependency bootstrap, and a one-keypress
*Alp: Generate all* command for the six loader emit modes.  Build
+ install locally:

```bash
cd vscode
npm install && npm run package
code --install-extension alp-sdk-*.vsix
```

Key tasks (Command Palette → **Tasks: Run Task**):

- `validate · metadata` — runs `validate_metadata.py`.
- `regen · soc_caps.h` / `regen · ABI snapshot` — regenerate
  generated artefacts after touching `metadata/` or `include/`.
- `twister · all` — runs the full ztest + example suite under
  `native_sim`.
- `west build · edgeai-vision-aen` / `iot-connected-camera` —
  builds the end-to-end reference apps.

## 11. Where to go next

- **[`docs/board-config.md`](board-config.md)** -- the authoritative
  `board.yaml` schema reference + recipe table for every loader
  emit mode (`zephyr-conf`, `cmake-args`, `yocto-conf`,
  `dts-overlay`, `hw-info-h`, `west-libraries`).  Start here when
  you're ready to write your own app's `board.yaml`.
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
  EEPROM-manifest + BOARD_ID-ADC API; `scripts/program_eeprom.py`
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
