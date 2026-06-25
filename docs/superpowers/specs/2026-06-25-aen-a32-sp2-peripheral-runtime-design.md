# SP2 — AEN A32 `alp_*` peripheral runtime on the booted image

**Date:** 2026-06-25
**Branch:** `feat/aen-a32-yocto-bringup` · **PR:** #264
**Predecessor:** SP1 (`2026-06-25-aen-a32-yocto-bringup-design.md`) — carrier dtb +
`alif-tiny-image` build end-to-end for `MACHINE=e1m-aen801-a32`, `libalp-sdk0`
installed in the rootfs, carrier DTS = real E1M-EVK mapping (console UART5,
sensors I2C2, disp I2C1).

## Goal

Prove the `src/yocto` `alp_*` peripheral backends (i2c / gpio, already cross-built
into `libalp_sdk.so`) drive the **E1M-EVK carrier peripherals** over the Linux
userspace ABIs on the booted A32 image — by shipping one bring-up example in the
rootfs. Everything code-side is done now; only the runtime probe is board-gated.

## Non-goals

- Runtime execution on hardware (no board — bench-gated).
- SPI / UART exercises (the console UART is already the boot path; SPI has no
  grounded EVK target on the sensor cluster). i2c + gpio cover the prompt's
  mapping ask.
- Kernel IIO/input driver bindings for the sensors. The `alp_i2c_*` backend talks
  **raw `/dev/i2c-N` via `I2C_RDWR`**, so no in-kernel device nodes are required;
  adding them is out of scope for SP2.

## Grounding (authoritative, nothing invented)

All addresses transcribed from `metadata/boards/e1m-evk.yaml` +
`include/alp/boards/alp_e1m_evk.h`:

| Device | Addr | Bus | Source |
|---|---|---|---|
| TCAL9538 IO-expander | `0x72` | `EVK_I2C_BUS_SENSORS` (E1M_I2C0) | e1m-evk.yaml `tcal9538: true` |
| PCIe TCAL9538 #2 | `0x71` | same | alp_e1m_evk.h (not used by SP2) |
| BMI323 IMU (primary) | `0x68` | same | e1m-evk.yaml `primary_imu: bmi323` |
| ICM-42670 IMU (alt) | `0x68` | same | e1m-evk.yaml (strap-selected sibling) |
| INA236 ×6 | `0x40..0x46` | same | e1m-evk.yaml `ina236: true` (not used by SP2) |

Bus mapping (SP1 carrier DTS): `EVK_I2C_BUS_SENSORS = E1M_I2C0` → Alif **I2C2**.
GPIO macros: `EVK_PIN_LED_GREEN` (E1M_GPIO_PWM0 pad as digital GPIO, output),
`EVK_PIN_BMI323_INT1` (E1M_GPIO_IO15, input).

Chip drivers all present and building in `libalp_chips.a`: `chips/bmi323`,
`chips/icm42670`, `chips/ina236`, `chips/tcal9538`.

## Linux ABI binding facts (from the backends)

- **`peripheral_i2c.c`**: `alp_i2c_config_t.bus_id` → trailing int of `/dev/i2c-N`.
  `bitrate_hz` ignored (kernel owns bus speed via DTS). Write-then-read uses
  `I2C_RDWR` 2-message (repeated-start).
- **`peripheral_gpio.c`**: gpiochip **v2 chardev** ABI (`/dev/gpiochipN`, no
  libgpiod). Flat `pin_id` packs the pair: `pin_id = (chip << 16) | line_offset`.
  IRQs via a shared `poll()` dispatcher thread.

## Components

### 1. `examples/aen/aen-a32-carrier-bringup/`

A32-Linux userspace app, plain-C + CMake, modeled on
`examples/v2n/v2n-power-monitor` (NOT a Zephyr example — no `prj.conf` /
`boards/`; the M55 `aen-*-regcheck` neighbors are Zephyr, this one is Linux and
its README says so up front).

Files:
- `src/main.c` — the bring-up sequence (below).
- `CMakeLists.txt` — `find_library(alp_chips)` + `find_library(alp_sdk)`,
  `find_path(alp/peripheral.h)`; `libalp_chips.a` links **before** `libalp_sdk.so`
  so the chip objects' `alp_*` callbacks resolve. Yocto-SDK / OE-toolchain build.
  `install(TARGETS ... DESTINATION bin)`.
- `board.yaml` — binds `board: e1m-evk`, `machine: e1m-aen801-a32`.
- `README.md` — what/why, the address table above, the SDK build command, and an
  explicit **board-gated** section listing the deferred values.

`main.c` sequence (each step prints a PASS/FAIL line; non-fatal — continues):
1. `alp_i2c_open(EVK_I2C_BUS_SENSORS)`.
2. **Bus scan** — probe `0x08..0x77`, print the ACKing set.
3. **TCAL9538 @0x72** via the `tcal9538` chip driver: configure one port pin as
   output, drive hi/lo, read the input port back.
4. **BMI323 @0x68** via the `bmi323` chip driver: read the chip-id / WHO_AM_I
   register, verify it matches BMI323; if it reads the ICM-42670 id instead, log
   that the strap selected the alternate IMU (runtime disambiguation — no guess).
5. **SoC GPIO** via `alp_gpio_*`: drive `EVK_PIN_LED_GREEN`, read
   `EVK_PIN_BMI323_INT1`. The packed `pin_id` for each is a board-gated constant
   (see below); a `gpiodetect`-style scan helper lists `/dev/gpiochipN` so the
   bench operator can fill them in.

### 2. `meta-alp-sdk/recipes-examples/aen-a32-carrier-bringup_0.6.bb`

First recipe under `recipes-examples/` (establishes the pattern; v2n-power-monitor
is SDK-built, not baked).
- `inherit cmake`.
- `DEPENDS = "alp-sdk alp-chips"` (headers + `libalp_sdk.so` + `libalp_chips.a` at
  build); `RDEPENDS:${PN} = "alp-sdk"` (rootfs needs only the shared lib — the chip
  lib is static).
- `SRC_URI` = the alp-sdk git repo, branch `feat/aen-a32-yocto-bringup`
  (mirrors `SRC_URI:pn-alp-sdk`); `S = ${WORKDIR}/git/examples/aen/aen-a32-carrier-bringup`,
  `SRCREV = "${AUTOREV}"`.
- Installs `bin/aen-a32-carrier-bringup`.

### 3. `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf`

Append the example to the image:
`IMAGE_INSTALL:append = " aen-a32-carrier-bringup"` (next to the existing
` alp-sdk`).

## Build / BBMASK interaction

SP1's mask is `BBMASK = "/meta-alp-sdk/recipes-ros/ /meta-alp-sdk/recipes-deepx/
/meta-alp-sdk/recipes-images/"`. `recipes-examples/` is **not** matched — confirm
during the bake. `BB_DANGLINGAPPENDS_WARNONLY = "1"` stays. `DISTRO=apss-tiny`
(DISTRO=alp is Renesas-hardwired). `auto.conf` written **after** the layer-add
loop. Drive bitbake via a `.sh` run through
`MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`.

## Validation (no-board)

1. Author the example; host compile-check on x86 WSL (the `__linux__`-guarded
   backends build natively) for a fast inner loop.
2. `bitbake aen-a32-carrier-bringup` (cortexa32 musl) — recipe parses + the ELF
   builds and links `libalp_sdk.so` + `libalp_chips.a`.
3. Rebake `alif-tiny-image` (MACHINE=e1m-aen801-a32) — assert
   `aen-a32-carrier-bringup` appears in the rootfs manifest and the installed ELF
   is `cortexa32 musl`.

## Board-gated (deferred to bench, marked `TODO(e1m-evk-hw)` in code)

- **i2c adapter index N** for `EVK_I2C_BUS_SENSORS` (kernel adapter enumeration;
  DTS makes it Alif I2C2 but `/dev/i2c-N` numbering is runtime).
- **gpiochip packed `pin_id`** for `EVK_PIN_LED_GREEN` + `EVK_PIN_BMI323_INT1`
  (`(chip<<16)|offset` resolved once `/dev/gpiochipN` enumerates on the board).
- **IMU strap** confirm at `0x68` (bmi323 vs icm42670) — runtime WHO_AM_I.
- **Actual runtime probe** of the full sequence.

## Risks

- The `recipes-examples/` SRC pulling `${AUTOREV}` of the branch means the bake
  fetches the live tip; acceptable for a bring-up branch, same as `pn-alp-sdk`.
- The example links `libalp_chips.a` static — a missing chip object surfaces only
  at link, caught by validation step 2.
