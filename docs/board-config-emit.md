# `board.yaml` build-artefact emission

How `scripts/alp_project.py` compiles a resolved `board.yaml` into
the per-backend native config: the `tan build` entry point, the
Zephyr `alp.conf` overlay, plain-CMake `-D` args, the Yocto
`local.conf` snippet, `west.yml` library auto-pinning, the
build-time `hw-info-h` identifier header, and the DTS overlay for
board wiring.

See [`docs/board-config.md`](board-config.md) for the landing page.

## How the loader compiles the file

`scripts/alp_project.py` reads `board.yaml`, validates against the
schema, resolves the SoM SKU + board presets, applies overrides,
and emits the requested build artifacts.  Common workflows below.

### West extension -- validate, generate, and build

The SDK ships first-class west extension commands via
[`scripts/west-commands.yml`](../scripts/west-commands.yml).  The
usual application build entry point is:

```bash
tan build -b <board> <app-dir>
```

`tan build` validates `<app-dir>/board.yaml`, emits the generated
per-slice configuration and system manifest, then dispatches the
underlying Zephyr / Yocto / baremetal build steps for the enabled
cores.  Companion commands (`tan image`, `tan flash`,
`tan clean`, `tan generate`, `west alp-size`, and
`tan renode`) consume the same build state for bundle, flash,
inspection, sizing, and simulation workflows.

### Zephyr -- generated `alp.conf` appended to `prj.conf`

The canonical pattern is to run the loader at configure time and
include the generated fragment from `prj.conf`:

```bash
# At your app root, alongside board.yaml + prj.conf:
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit zephyr-conf \
    --output build/generated/alp.conf
```

The application's `CMakeLists.txt` wires the loader as a
configure-time step and layers the result over `prj.conf` with
`OVERLAY_CONFIG`.  `prj.conf` itself stays empty -- everything
flows from `board.yaml`:

```cmake
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Point ALP_SDK_ROOT at your alp-sdk checkout (env override, else a
# tree-relative fallback -- mirrors the examples/*/CMakeLists.txt pattern).
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
endif()

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rv=${_alp_rv})")
endif()
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app LANGUAGES C)
target_sources(app PRIVATE src/main.c)
```

Zephyr's `OVERLAY_CONFIG` machinery merges the generated `alp.conf`
on top of `prj.conf` at Kconfig time -- the app picks up every
`CONFIG_*` line the loader emitted.

### Plain CMake (baremetal / yocto) -- generated `-D` args

```bash
# Pipe the generated args straight into your configure step:
ARGS=$(python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit cmake-args)
cmake -B build $ARGS .
```

Or wire the loader into a `CMakeUserPresets.json` writer if your
build system already drives presets.

### Yocto -- generated `local.conf` snippet

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit yocto-conf \
    --output build/conf/alp-generated.conf
echo 'require alp-generated.conf' >> build/conf/local.conf
```

### west.yml libraries auto-pin (`--emit west-libraries`)

`--emit west-libraries` produces a `west.yml` fragment listing the
Zephyr modules and exact west project pins the board.yaml's library
declarations require.  Zephyr-owned modules (`lvgl`, `mbedtls`,
`cmsis-dsp`, `fs/littlefs`) land in the `zephyr` import
`name-allowlist:`.  ADR 0018 libraries whose manifests carry an
`integration.zephyr.west` pin, such as `aws-iot` and `azure-iot`,
land as concrete `projects:` entries with exact upstream release
tags.  Header-only C++ libraries (`etl`, `fmt`, `nlohmann_json`,
`doctest`) aren't Zephyr modules -- they ride the loader's
compile-time profile hook instead -- so the emitter lists them in a
trailing comment rather than the allowlist.

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit west-libraries \
    --output build/generated/alp-west-libs.yml
```

Import the fragment from your application's `west.yml`:

```yaml
manifest:
  projects:
    - name: alp-app-libs
      path: alp-app-libs
      import: build/generated/alp-west-libs.yml
```

Run `west update` and only the modules board.yaml actually
references land in the workspace.  Closes the second v0.4 gap
this doc previously flagged.

### Build-time identifier header (`--emit hw-info-h`)

`--emit hw-info-h` produces an auto-generated companion header to
[`<alp/hw_info.h>`](../include/alp/hw_info.h) that bakes the
customer's board.yaml identifiers into `ALP_HW_BUILD_*` string
macros.  Apps include both headers and pass the compile-time
constants to the runtime check:

```c
#include "alp/hw_info.h"
#include "alp_hw_info_build.h"   /* generated */

alp_hw_info_t info;
alp_hw_info_read(&info);
alp_hw_info_assert_matches_build(&info,
                                 ALP_HW_BUILD_SOM_SKU,
                                 ALP_HW_BUILD_SOM_HW_REV);
```

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit hw-info-h \
    --output build/generated/alp_hw_info_build.h
```

Macros emitted: `ALP_HW_BUILD_SOM_SKU`, `ALP_HW_BUILD_SOM_FAMILY`,
`ALP_HW_BUILD_SOM_HW_REV`, `ALP_HW_BUILD_OS`, and (when a board
is declared) `ALP_HW_BUILD_BOARD_NAME` +
`ALP_HW_BUILD_BOARD_HW_REV`.

### DTS overlay for board wiring (v0.3)

`--emit dts-overlay` reads the board header at
`include/alp/boards/<board>.h`, finds every `#define X
E1M_<class><N>` macro for the v0.3-scoped classes (I2C, SPI,
UART, PWM, GPIO_IO), and emits a Zephyr `.overlay` declaring:

- One alias per bus channel the board wires (`alp-i2c<N> =
  &i2c<N>;`, `alp-spi<N> = &spi<N>;`, `alp-uart<N> = &uart<N>;`,
  `alp-pwm<N> = &pwm<N>;`).  The trailing comment on each alias
  lists every macro in the board header that references that
  channel.
- An `alp_pins` node with `compatible = "alp,pin-array"` and one
  `gpios` entry per `EVK_PIN_*` macro that resolves to an
  `ALP_E1M_GPIO_IO<N>`.  Each entry's `<&gpioX Y FLAGS>` triplet
  is a TBD placeholder; the trailing comment carries the macro
  name and the E1M IO index so the customer can fill the columns
  in place without renumbering.

```bash
python3 $ALP_SDK/scripts/alp_project.py \
    --input board.yaml \
    --emit dts-overlay \
    --output build/generated/alp.overlay
```

The dts-overlay emitter still synthesizes only the bus aliases and
GPIO pin array above.  Display devices are supported through the
Zephyr devicetree path today, but the board preset or app overlay
must provide the concrete display node.  For SPI TFTs, use Zephyr's
MIPI DBI Type C binding (`compatible = "zephyr,mipi-dbi-spi"`) with
the panel driver child (for example `sitronix,st7789v`), then expose
that child as `zephyr,display` for LVGL and/or `alp-display0` for
`<alp/display.h>`.  Application code stays on LVGL or the portable
display API; it does not initialise the panel driver directly.

First-class display synthesis in the dts-overlay emitter remains a
follow-up once the upstream SoM board files lock the gpio bank/index
columns.  Encoders, cameras, and other non-bus device classes follow
the same rule.

### What the loader does NOT yet do

- **Cross-validation against `metadata/socs/*.json`** -- the loader
  trusts the SKU preset's `silicon:` field; it doesn't yet
  validate that requested features (e.g. 16-bit ADC) match the
  SoC's documented caps.  The `<alp/soc_caps.h>` runtime check
  catches mismatches at `_open` time today.

