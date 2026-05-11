# Getting started with the ALP SDK

This walkthrough takes you from "git clone" to a working
`gpio-button-led` build under `native_sim`, then onto real
silicon.  No `alp-studio` required — the SDK supports
hand-written firmware as a first-class consumer.

If you'd rather skim, the fastest path is:

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk
bash scripts/bootstrap.sh                            # one-time: west + Python + apt hints
export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
west alp-build -b native_sim/native/64 examples/gpio-button-led
west build -d build -t run
# expect: [gpio] init button=ALP_E1M_GPIO_IO0, led=ALP_E1M_GPIO_IO1
#          ...
#          [gpio] done
```

`scripts/bootstrap.sh` is the canonical fresh-clone setup -- it
creates the Zephyr workspace one level up from `alp-sdk/`, runs
`west update --narrow`, installs the Zephyr Python deps + the
SDK's extras (`jsonschema`, `imgtool`), and prints OS-specific
`apt` / `brew` commands for the optional native libraries the
Yocto-side backends need.

`west alp-build` validates the example's `board.yaml`, generates
the build-time config from it, and delegates to `west build`.
The rest of this document explains *why* each step is what it is
so you can adapt it to your own project.

For a full local verification pass (everything CI runs short of
real-hardware HIL), see [`docs/testing.md`](testing.md):

```bash
bash scripts/test-all.sh
```

## 1. Prerequisites

The SDK is supported equally on **macOS**, **Windows**, and
**Linux** -- pick whichever you already have.  Tooling versions
are identical across hosts; the only platform-specific bit is how
you install them.

| Tool        | Version          | Notes                                                    |
|-------------|------------------|----------------------------------------------------------|
| Zephyr      | v3.7.0 LTS       | Pinned by `west.yml`; see [`docs/zephyr-version-policy.md`](zephyr-version-policy.md). |
| Python      | 3.10+            | For `west`, the v0.3 loader (`alp_project.py`), validators. |
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
west init -m https://github.com/alplabai/alp-sdk
west update --narrow -o=--depth=1
west zephyr-export
```

After this:

- `alp-workspace/zephyr/`    Zephyr v3.7.0 LTS (pinned via the SDK's `west.yml`).
- `alp-workspace/modules/`   Zephyr's standard modules (HAL, libs).
- `alp-workspace/alp-sdk/`   This repo, mounted as a Zephyr module.

`west update --narrow -o=--depth=1` keeps the clone shallow —
saves ~30 GB of unrelated git history.

Importing alp-sdk via `west init -m` also surfaces the
`west alp-build` extension command (see `scripts/west_commands/alp.py`)
that the rest of this walkthrough uses.

## 4. First build: the GPIO example

Every example in `examples/` carries a **`board.yaml`** — the
single declarative file the loader compiles into Kconfig
fragments, DTS overlays, and the build-time hw_info header.  The
`west alp-build` wrapper does the pre-flight + delegates to
`west build`:

```bash
cd alp-workspace
west alp-build -b native_sim/native/64 alp-sdk/examples/gpio-button-led
```

What the flags mean:

- `-b native_sim/native/64` — Zephyr's POSIX simulator on a 64-bit
  host.  No silicon needed; runs as a native process on Linux /
  macOS / WSL.  (Native Windows: use `west alp-build -b native_sim/native/32`
  with the MSVC-compatible toolchain, or run the 64-bit variant
  through WSL.)
- `alp-sdk/examples/gpio-button-led` — the application directory.
  Each example under `examples/` ships a `board.yaml` + an empty
  `prj.conf` + a CMakeLists.txt that invokes the loader at
  configure time.  See [`docs/board-config.md`](board-config.md)
  for the schema.

`west alp-build` walks four steps under the hood:

1. **Validates** the app's `board.yaml` via `validate_board_yaml.py`
   (schema + SoM SKU preset + carrier preset + `hw_rev` /
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
          uart-echo uart-rx-ringbuf counter-alarm rtc-clock wdt-feed \
          can-loopback i2s-tone qenc-readout; do
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

## 8. SoC capability validation

In v0.3 the SoC choice flows from `board.yaml`'s `som.sku` field
automatically -- the loader resolves the MPN to the silicon ref
(`alif:ensemble:e7` for `E1M-AEN701`) and emits the matching
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

## 9. Editing in VS Code

Two complementary surfaces:

**The SDK's `.vscode/` config** (`extensions`, `settings`, `tasks`,
`c_cpp_properties`) is set up for Zephyr-module + plain-CMake
development.  See the "Using with VS Code" section in
[`README.md`](../README.md).

**The `alplabai.alp-sdk` extension** under [`vscode/`](../vscode/)
adds schema-aware `board.yaml` editing (autocomplete on SKUs,
carriers, libraries; inline diagnostics from `validate_board_yaml.py`
in the Problems panel), a GUI configurator panel with dropdowns
for every released MPN + carrier, west wrappers (build / flash /
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

## 10. Where to go next

- **[`docs/board-config.md`](board-config.md)** -- the authoritative
  `board.yaml` schema reference + recipe table for every loader
  emit mode (`zephyr-conf`, `cmake-args`, `yocto-conf`,
  `dts-overlay`, `hw-info-h`, `west-libraries`).  Start here when
  you're ready to write your own app's `board.yaml`.
- **Per-peripheral examples**: [`examples/`](../examples/README.md)
  -- 11 minimal apps, one per `<alp/*.h>` class, each driven by a
  matching `board.yaml`.
- **End-to-end reference apps**:
  [`examples/edgeai-vision-aen/`](../examples/edgeai-vision-aen/)
  (camera → Ethos-U inference → display) and
  [`examples/iot-connected-camera/`](../examples/iot-connected-camera/)
  (camera → DRP-AI → MQTT publish).  Both use the same board.yaml
  workflow at a larger scale.
- **Hardware identification + production-test**:
  [`<alp/hw_info.h>`](../include/alp/hw_info.h) for the runtime
  EEPROM-manifest + BOARD_ID-ADC API; `tools/program_eeprom.py`
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
