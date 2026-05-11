# Getting started with the ALP SDK

This walkthrough takes you from "git clone" to a working
`gpio-button-led` build under `native_sim`, then onto real
silicon.  No `alp-studio` required — the SDK supports
hand-written firmware as a first-class consumer.

If you'd rather skim, the fastest path is:

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk
west init -l .                                  # one-time
west update                                     # one-time, ~10 min
west build -b native_sim/native/64 examples/gpio-button-led \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
# expect: [gpio] init button=ALP_E1M_GPIO_IO0, led=ALP_E1M_GPIO_IO1
#          ...
#          [gpio] done
```

The rest of this document explains *why* each step is what it is
so you can adapt it to your own project.

## 1. Prerequisites

The SDK is supported equally on **macOS**, **Windows**, and
**Linux** -- pick whichever you already have.  Tooling versions
are identical across hosts; the only platform-specific bit is how
you install them.

| Tool        | Version          | Notes                                                    |
|-------------|------------------|----------------------------------------------------------|
| Zephyr      | v3.7.0           | Pinned by `west.yml`; v0.1 SDK matrix (see VERSIONS.md). |
| Python      | 3.10+            | For `west`, `gen_soc_caps.py`, `validate_metadata.py`.   |
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

For real-silicon builds you'll also need the Zephyr SDK
(`zephyr-sdk-0.17.0` is the v0.1 SDK matrix's pin) and a JTAG /
SWD probe matching your board.  See
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md) for the EVK's wiring.

> **Note for Windows users.**  The repo's `.gitattributes` pins
> LF on every source file -- a fresh clone gets identical bytes
> on PowerShell, WSL, and macOS / Linux, so clang-format-diff CI
> doesn't trip on a Windows checkout.

## 2. Two consumer paths

The SDK supports both flows equally — pick whichever fits.  ADR
[0001](adr/0001-wrapper-on-top-of-zephyr.md) explains the
rationale.

### 2.1 Standalone (this walkthrough)

You write Zephyr / Yocto / bare-metal app code directly against
`<alp/...>` headers.  Pick instance IDs by hand from
`<alp/e1m_pinout.h>` (`ALP_E1M_I2C0`, `ALP_E1M_PWM3`, …).  Your
app stays portable across every E1M-conformant SoM.  The rest
of this document covers this path.

### 2.2 alp-studio codegen

The studio reads block manifests, runs the pin allocator, and
emits the same `<alp/...>` calls.  Switching to studio at any
point is non-destructive — your standalone app keeps working
alongside the studio-generated code.  See
[`alplabai/alp-studio`](https://github.com/alplabai/alp-studio).

## 3. Set up a Zephyr workspace

The SDK is a Zephyr **module** — your build pulls Zephyr,
modules, and the SDK in one go via `west`.

```bash
mkdir alp-workspace && cd alp-workspace
west init -m https://github.com/alplabai/alp-sdk --mr main
west update --narrow -o=--depth=1
west zephyr-export
```

After this:

- `alp-workspace/zephyr/`    Zephyr v3.7.0 (pinned).
- `alp-workspace/modules/`   Zephyr's standard modules (HAL, libs).
- `alp-workspace/alp-sdk/`   This repo, mounted as a Zephyr module.

`west update --narrow -o=--depth=1` keeps the clone shallow —
saves ~30 GB of unrelated git history.

## 4. First build: the GPIO example

```bash
cd alp-workspace
west build -b native_sim/native/64 \
    alp-sdk/examples/gpio-button-led \
    -d build/gpio-button-led
```

What the flags mean:

- `-b native_sim/native/64` — Zephyr's POSIX simulator on a 64-bit
  host.  No silicon needed; runs as a native process on Linux /
  macOS / WSL.  (Native Windows: use `west build -b native_sim/native/32`
  with the MSVC-compatible toolchain, or run the 64-bit variant
  through WSL.)
- `examples/gpio-button-led` — the application directory.  Each
  example under `examples/` is self-contained (CMakeLists,
  prj.conf, board overlay, src/main.c).
- `-d build/gpio-button-led` — separate build dir per example.
  Avoids stale-build confusion when you switch examples.

Run it:

```bash
west build -d build/gpio-button-led -t run
```

Expected output:

```
*** Booting Zephyr OS build v3.7.0 ***
[gpio] init button=ALP_E1M_GPIO_IO0, led=ALP_E1M_GPIO_IO1
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

Open `alp-sdk/examples/gpio-button-led/src/main.c`.  Every
example app is annotated as teaching material — the comments
spell out:

- What each `alp_*_open` config field means and what the alternative
  values would do.
- Why specific values were chosen (timing, geometry, mode).
- What `alp_last_error()` returns on each failure mode.
- Lifecycle expectations of `*_close`.

Read the `gpio-button-led` example through, then look at
`<alp/chips/button_led.h>` to see what API the example is
calling into.  The button_led helper (`alp_button_led_*`) is
SDK-level; the underlying primitives (`alp_gpio_*`) are in
`<alp/peripheral.h>`.

## 6. Run more examples

Every wrapped peripheral has a corresponding example:

```bash
for ex in pwm-led-fade adc-voltmeter i2c-scanner spi-loopback \
          uart-echo counter-alarm rtc-clock wdt-feed can-loopback \
          i2s-tone qenc-readout; do
    west build -b native_sim/native/64 alp-sdk/examples/$ex \
        -d build/$ex
    west build -d build/$ex -t run
done
```

On `native_sim` most peripherals don't have emul controllers
(only I²C / SPI / GPIO / UART do).  The examples that target
unwrapped peripherals exit after printing the
`alp_last_error()` diagnostic — that's expected and proves the
wrapper plumbing compiles + links cleanly.

## 7. Targeting real silicon

When `alplabai/alp-zephyr-modules` publishes the EVK board
files (`alp_e1m_evk_aen` for the AEN family, `alp_e1m_evk_v2n`
for V2N), build with that as the `-b` target:

```bash
west build -b alp_e1m_evk_aen alp-sdk/examples/gpio-button-led
west flash
```

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
   carrier-specific pin remapping.

`docs/porting-new-som.md` covers the full porting checklist.

## 8. SoC capability validation (optional)

The SDK validates `*_open` configs against the active SoC's
documented hardware caps.  Without telling the build which SoC
you're on, the validation passes through (default macros are
`UINT16_MAX`).  To opt in:

```kconfig
# In your prj.conf
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y    # or RENESAS_RZV2N_N44, etc.
```

After this, `alp_adc_open` with `resolution_bits = 16` on E3
(12-bit max) returns NULL with `alp_last_error()` =
`ALP_ERR_OUT_OF_RANGE`.  See ADR
[0002](adr/0002-error-mechanism.md) for the diagnostic
contract.

## 9. Editing in VS Code

The repo ships `.vscode/{extensions,settings,tasks,c_cpp_properties}.json`
configured for Zephyr-module + plain-CMake development.  See the
"Using with VS Code" section in [`README.md`](../README.md).

Key tasks (Command Palette → **Tasks: Run Task**):

- `validate · metadata` — runs `validate_metadata.py`.
- `regen · soc_caps.h` / `regen · ABI snapshot` — regenerate
  generated artefacts after touching `metadata/` or `include/`.
- `twister · all` — runs the full ztest + example suite under
  `native_sim`.
- `west build · edgeai-vision-aen` / `iot-connected-camera` —
  builds the end-to-end reference apps.

## 10. Where to go next

- **Project configuration** -- `board.yaml` is the **single source
  of truth** for your firmware's target.  The minimum-viable
  config is three lines: pick your **MPN** (e.g. `E1M-AEN701`),
  pick your **carrier** (e.g. `E1M-EVK`), pick your **OS**
  (`zephyr` / `yocto` / `baremetal`).  Everything else flows from
  the SDK-shipped per-MPN preset at
  `metadata/e1m_modules/<MPN>/som.yaml`.  Every other config
  artefact (Zephyr `prj.conf`, CMake `-D` args, Yocto
  `local.conf`) is derived output -- don't hand-edit them.  Read
  [`docs/board-config.md`](board-config.md) for the model + the
  list of every MPN the SDK ships a preset for, then copy
  [`metadata/templates/board.yaml`](../metadata/templates/board.yaml)
  to your app root and edit.
- Per-peripheral examples: [`examples/`](../examples/README.md)
- End-to-end reference apps:
  [`examples/edgeai-vision-aen/`](../examples/edgeai-vision-aen/)
  and [`examples/iot-connected-camera/`](../examples/iot-connected-camera/)
- **Recommended third-party libraries** to pair with the SDK
  (CMSIS-DSP, ETLCPP, fmt, nlohmann/json, doctest, LittleFS, and
  more): [`docs/recommended-libraries.md`](recommended-libraries.md).
  The SDK deliberately stays small; that doc is the curated list
  of battle-tested libraries that fill the gaps for app code.
- Architecture overview: [`docs/architecture.md`](architecture.md)
- Architecture decision records: [`docs/adr/`](adr/)
- Hardware specs: [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec)
- Per-version roadmap: [`VERSIONS.md`](../VERSIONS.md)
- Contributor guide: [`CONTRIBUTING.md`](../CONTRIBUTING.md)
