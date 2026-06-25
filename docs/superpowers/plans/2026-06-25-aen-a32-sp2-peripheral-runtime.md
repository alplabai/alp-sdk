# SP2 — AEN A32 `alp_*` Peripheral Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one A32-Linux bring-up example in the `e1m-aen801-a32` rootfs that drives the E1M-EVK carrier peripherals through the portable `alp_*` API over the Linux userspace ABIs.

**Architecture:** A plain-C/CMake example (`examples/aen/aen-a32-carrier-bringup/`) modeled on `examples/v2n/v2n-power-monitor`, linking `libalp_chips.a` (static) before `libalp_sdk.so` (shared). A new `meta-alp-sdk/recipes-examples/` recipe builds it for cortexa32/musl and `IMAGE_INSTALL`s it into `alif-tiny-image`. All device addresses come from `<alp/boards/alp_e1m_evk.h>`; board-gated runtime values (i2c adapter index, gpiochip packed pin_id) are isolated as named `TODO(e1m-evk-hw)` constants so the code compiles + links now.

**Tech Stack:** C11, CMake ≥3.20, alp-sdk `<alp/peripheral.h>` + chip drivers (`tcal9538`, `bmi323`, `icm42670`), Yocto/OE (scarthgap, `DISTRO=apss-tiny`), bitbake 2.8, WSL Ubuntu.

## Global Constraints

- Copyright header on every new source file: `Copyright 2026 Alp Lab AB` + `SPDX-License-Identifier: Apache-2.0` (exact spelling "Alp Lab", not "ALP Lab").
- No invented pins/addresses/straps — transcribe from `alp_e1m_evk.h` / `e1m-evk.yaml`, else mark `TODO(e1m-evk-hw)`.
- Core peripherals go through the portable `alp_*` surface only (`alp_i2c_*`, `alp_gpio_*`); chip access via the in-repo `chips/<part>/` drivers.
- No `Co-Authored-By: Claude` footer on commits; attribute to alpCaner.
- Drive bitbake via a `.sh` run through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh` — never inline `wsl bash -lc`. Write `auto.conf` AFTER the layer-add loop.
- Yocto build: `DISTRO=apss-tiny`, `MACHINE=e1m-aen801-a32`, `BBMASK = "/meta-alp-sdk/recipes-ros/ /meta-alp-sdk/recipes-deepx/ /meta-alp-sdk/recipes-images/"`, `BB_DANGLINGAPPENDS_WARNONLY = "1"`.
- WSL build env: `~/alif-bsp-ref/alif_linux-apss-build-setup` (branch `scarthgap_yocto_5.0`); resume per `project_e1m_aen_a32_yocto_bringup` memory.

---

### Task 1: Author the bring-up example sources

**Files:**
- Create: `examples/aen/aen-a32-carrier-bringup/src/main.c`
- Create: `examples/aen/aen-a32-carrier-bringup/CMakeLists.txt`
- Create: `examples/aen/aen-a32-carrier-bringup/board.yaml`
- Create: `examples/aen/aen-a32-carrier-bringup/README.md`

**Interfaces:**
- Consumes (from the installed SDK, verified signatures):
  - `alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg);` where `alp_i2c_config_t { uint32_t bus_id; uint32_t bitrate_hz; }`
  - `alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len);`
  - `void alp_i2c_close(alp_i2c_t *bus);`
  - `alp_gpio_t *alp_gpio_open(uint32_t pin_id);`
  - `alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull);` with `ALP_GPIO_INPUT/ALP_GPIO_OUTPUT`, `ALP_GPIO_PULL_NONE`
  - `alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level);` / `alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level);` / `void alp_gpio_close(alp_gpio_t *pin);`
  - `tcal9538_init(tcal9538_t*, alp_i2c_t*, uint8_t)`, `tcal9538_set_direction(.., uint8_t pin, TCAL9538_DIR_OUTPUT)`, `tcal9538_set(.., pin, bool)`, `tcal9538_read_all(.., uint8_t*)`, `tcal9538_deinit(..)`
  - `bmi323_init(bmi323_t*, alp_i2c_t*, uint8_t)`, `bmi323_read_id(bmi323_t*, uint8_t*)`, `bmi323_deinit(..)`, macro `BMI323_CHIP_ID` (0x43)
  - `icm42670_init(icm42670_t*, alp_i2c_t*, uint8_t)`, `icm42670_read_id(..)`, `icm42670_deinit(..)`, macro `ICM42670_WHO_AM_I_VAL` (0x67)
  - Board macros: `EVK_I2C_ADDR_TCAL9538` (0x72), `EVK_I2C_ADDR_BMI323` (0x68), `EVK_I2C_ADDR_ICM42670` (0x69), `ALP_OK`
- Produces: the binary `aen-a32-carrier-bringup` (consumed by Task 2's recipe).

- [ ] **Step 1: Write `src/main.c`**

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * aen-a32-carrier-bringup -- exercise the E1M-EVK carrier peripherals
 * from a Linux/Yocto user-space app on the E1M-AEN801 Cortex-A32.
 *
 * Demonstrates the portable alp_* surface mapping onto the Linux
 * userspace ABIs:
 *   - alp_i2c_* over /dev/i2c-N (i2c-dev): bus scan + chip drivers
 *   - tcal9538 IO-expander @0x72 toggle/read
 *   - bmi323 (0x68) / icm42670 (0x69) IMU chip-id, runtime-detected
 *   - alp_gpio_* over the gpiochip v2 chardev ABI
 *
 * Bring-up / demo utility, not a production path.  Addresses come
 * from <alp/boards/alp_e1m_evk.h>; nothing is invented.
 *
 * Board-gated values (TODO(e1m-evk-hw)) are isolated as named
 * constants -- the code compiles + links now; fill them in on the
 * bench from the live /dev enumeration.  See README.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/icm42670.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/boards/alp_e1m_evk_routes.h"

/*
 * Board-gated runtime mappings -- TODO(e1m-evk-hw).
 *
 * The SP1 carrier DTS routes EVK_I2C_BUS_SENSORS (E1M_I2C0) to Alif
 * I2C2, but the kernel assigns the /dev/i2c-N adapter index at boot.
 * Confirm with `i2cdetect -l` on the target and set N to match.
 */
#define AEN_SENSOR_I2C_ADAPTER 2u /* TODO(e1m-evk-hw): confirm /dev/i2c-N */

/*
 * alp_gpio_* packs pin_id = (gpiochip << 16) | line_offset.  Resolve
 * the gpiochip index + line offset for the LED + IMU-INT pads from the
 * live `gpiodetect` / `gpioinfo` enumeration on the booted board.
 */
#define AEN_GPIO_PACK(chip, line) (((uint32_t)(chip) << 16) | (uint32_t)(line))
#define AEN_PIN_LED_GREEN  AEN_GPIO_PACK(0, 0) /* TODO(e1m-evk-hw): EVK_PIN_LED_GREEN */
#define AEN_PIN_BMI323_INT AEN_GPIO_PACK(0, 0) /* TODO(e1m-evk-hw): EVK_PIN_BMI323_INT1 */

static int g_pass;
static int g_fail;

#define STEP_OK(msg)                                                                               \
	do {                                                                                       \
		printf("[ OK ] %s\n", (msg));                                                      \
		g_pass++;                                                                          \
	} while (0)
#define STEP_FAIL(msg)                                                                             \
	do {                                                                                       \
		printf("[FAIL] %s\n", (msg));                                                      \
		g_fail++;                                                                          \
	} while (0)

static void bus_scan(alp_i2c_t *bus)
{
	printf("-- i2c scan on adapter %u --\n", AEN_SENSOR_I2C_ADAPTER);
	for (uint8_t a = 0x08u; a <= 0x77u; a++) {
		uint8_t dummy = 0;
		if (alp_i2c_read(bus, a, &dummy, 1) == ALP_OK)
			printf("   device @ 0x%02x\n", a);
	}
}

static void probe_expander(alp_i2c_t *bus)
{
	tcal9538_t exp;
	if (tcal9538_init(&exp, bus, EVK_I2C_ADDR_TCAL9538) != ALP_OK) {
		STEP_FAIL("tcal9538 init @0x72");
		return;
	}
	uint8_t port = 0;
	if (tcal9538_set_direction(&exp, 0, TCAL9538_DIR_OUTPUT) == ALP_OK &&
	    tcal9538_set(&exp, 0, true) == ALP_OK && tcal9538_set(&exp, 0, false) == ALP_OK &&
	    tcal9538_read_all(&exp, &port) == ALP_OK) {
		printf("   tcal9538 port = 0x%02x\n", port);
		STEP_OK("tcal9538 toggle + read");
	} else {
		STEP_FAIL("tcal9538 toggle + read");
	}
	tcal9538_deinit(&exp);
}

static void probe_imu(alp_i2c_t *bus)
{
	bmi323_t bmi;
	if (bmi323_init(&bmi, bus, EVK_I2C_ADDR_BMI323) == ALP_OK) {
		uint8_t id = 0;
		bmi323_read_id(&bmi, &id);
		printf("   bmi323 chip-id = 0x%02x (expect 0x%02x)\n", id, BMI323_CHIP_ID);
		STEP_OK("primary IMU = bmi323 @0x68");
		bmi323_deinit(&bmi);
		return;
	}
	icm42670_t icm;
	if (icm42670_init(&icm, bus, EVK_I2C_ADDR_ICM42670) == ALP_OK) {
		uint8_t id = 0;
		icm42670_read_id(&icm, &id);
		printf("   icm42670 who_am_i = 0x%02x (expect 0x%02x)\n", id,
		       ICM42670_WHO_AM_I_VAL);
		STEP_OK("alternate IMU = icm42670 @0x69");
		icm42670_deinit(&icm);
		return;
	}
	STEP_FAIL("IMU (neither bmi323@0x68 nor icm42670@0x69 answered)");
}

static void probe_gpio(void)
{
	alp_gpio_t *led = alp_gpio_open(AEN_PIN_LED_GREEN);
	if (led != NULL && alp_gpio_configure(led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) == ALP_OK &&
	    alp_gpio_write(led, true) == ALP_OK) {
		alp_gpio_write(led, false);
		STEP_OK("SoC GPIO LED drive (gpiochip)");
	} else {
		STEP_FAIL("SoC GPIO LED drive (set AEN_PIN_LED_GREEN)");
	}
	if (led != NULL)
		alp_gpio_close(led);

	alp_gpio_t *intp = alp_gpio_open(AEN_PIN_BMI323_INT);
	bool lvl = false;
	if (intp != NULL && alp_gpio_configure(intp, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE) == ALP_OK &&
	    alp_gpio_read(intp, &lvl) == ALP_OK) {
		printf("   BMI323 INT1 line = %d\n", (int)lvl);
		STEP_OK("SoC GPIO INT read (gpiochip)");
	} else {
		STEP_FAIL("SoC GPIO INT read (set AEN_PIN_BMI323_INT)");
	}
	if (intp != NULL)
		alp_gpio_close(intp);
}

int main(void)
{
	printf("== aen-a32-carrier-bringup (E1M-EVK on E1M-AEN801) ==\n");

	alp_i2c_config_t cfg = {
		.bus_id     = AEN_SENSOR_I2C_ADAPTER,
		.bitrate_hz = 400000u,
	};
	alp_i2c_t *bus = alp_i2c_open(&cfg);
	if (bus == NULL) {
		STEP_FAIL("alp_i2c_open(sensor bus)");
		return 1;
	}
	STEP_OK("alp_i2c_open(sensor bus)");

	bus_scan(bus);
	probe_expander(bus);
	probe_imu(bus);
	alp_i2c_close(bus);

	probe_gpio();

	printf("== done: %d ok, %d fail ==\n", g_pass, g_fail);
	return (g_fail != 0) ? 1 : 0;
}
```

- [ ] **Step 2: Write `CMakeLists.txt`** (copied structure from `examples/v2n/v2n-power-monitor/CMakeLists.txt`)

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# Plain C build for the aen-a32-carrier-bringup demo.  Layered on top
# of the standard alp-sdk runtime (libalp_sdk.so) + on-board chip
# drivers (libalp_chips.a), both staged by the Yocto image's
# meta-alp-sdk layer.  Build inside a Yocto SDK environment that has
# sourced its environment-setup script (see README.md).

cmake_minimum_required(VERSION 3.20)
project(aen_a32_carrier_bringup LANGUAGES C)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# libalp_chips.a must link BEFORE libalp_sdk.so so the chip objects'
# alp_* peripheral callbacks resolve against the shared lib.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_library(ALP_CHIPS_LIB alp_chips REQUIRED)
find_path(ALP_SDK_INC alp/peripheral.h REQUIRED)

add_executable(aen-a32-carrier-bringup src/main.c)

target_include_directories(aen-a32-carrier-bringup PRIVATE ${ALP_SDK_INC})
target_link_libraries(aen-a32-carrier-bringup ${ALP_CHIPS_LIB} ${ALP_SDK_LIB})

install(TARGETS aen-a32-carrier-bringup DESTINATION bin)
```

- [ ] **Step 3: Write `board.yaml`** (matches `examples/v2n/v2n-power-monitor/board.yaml` shape — confirm its keys first with `cat examples/v2n/v2n-power-monitor/board.yaml` and mirror them)

```yaml
# SPDX-License-Identifier: Apache-2.0
# Binds this example to the E1M-EVK carrier on the AEN A32 Linux machine.
board: e1m-evk
machine: e1m-aen801-a32
```

- [ ] **Step 4: Write `README.md`**

````markdown
# aen-a32-carrier-bringup

Bring-up smoke test for the **E1M-EVK** carrier peripherals, run from a
Linux/Yocto user-space app on the **E1M-AEN801 Cortex-A32**. It walks
the portable `alp_*` surface over the Linux userspace ABIs:

- `alp_i2c_*` over `/dev/i2c-N` (i2c-dev): a `0x08..0x77` bus scan
- the `tcal9538` I/O-expander at `0x72` (toggle one output, read the port)
- the IMU at `0x68`/`0x69` (`bmi323` first, `icm42670` fallback — reports
  which is populated via CHIP_ID / WHO_AM_I)
- `alp_gpio_*` over the gpiochip v2 chardev ABI (drive the green LED, read
  the BMI323 INT1 line)

> **EVK-only / demo.** A bring-up utility, not a production path.
> Addresses come from `<alp/boards/alp_e1m_evk.h>`; nothing is invented.

## Build (Yocto SDK)

```sh
. /opt/poky/<ver>/environment-setup-arm-poky-linux-musleabi
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

The recipe `meta-alp-sdk/recipes-examples/aen-a32-carrier-bringup_0.6.bb`
also bakes the binary into `alif-tiny-image` (`/usr/bin/aen-a32-carrier-bringup`).

## Board-gated values (`TODO(e1m-evk-hw)` in `src/main.c`)

Fill these in on the bench from the live `/dev` enumeration, then rebuild:

| Constant | How to resolve |
|---|---|
| `AEN_SENSOR_I2C_ADAPTER` | `i2cdetect -l` → the `/dev/i2c-N` for Alif I2C2 |
| `AEN_PIN_LED_GREEN` | `gpioinfo` → `(gpiochip << 16) \| line` for the green LED pad |
| `AEN_PIN_BMI323_INT` | `gpioinfo` → packed pin_id for the BMI323 INT1 pad |

Until then the I2C steps run against the placeholder adapter and the GPIO
steps report `[FAIL]` with the constant to set — by design, not a defect.
````

- [ ] **Step 5: Host syntax-check on WSL** (catches API/signature errors against the real headers — no link needed)

Run:
```
MSYS_NO_PATHCONV=1 wsl bash -c "cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && cc -std=c11 -Wall -Wextra -Iinclude -fsyntax-only examples/aen/aen-a32-carrier-bringup/src/main.c"
```
Expected: no output, exit 0. If it errors on a missing symbol, fix the call against the header before continuing.

- [ ] **Step 6: Commit**

```bash
git add examples/aen/aen-a32-carrier-bringup/
git commit -m "feat(aen): A32-Linux carrier bring-up example (i2c scan + tcal9538 + IMU + gpio)"
```

---

### Task 2: meta-alp-sdk recipe + image install

**Files:**
- Create: `meta-alp-sdk/recipes-examples/aen-a32-carrier-bringup_0.6.bb`
- Modify: `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf` (the `IMAGE_INSTALL:append` line)

**Interfaces:**
- Consumes: the `aen-a32-carrier-bringup` binary built by Task 1's CMakeLists; the `alp-sdk` + `alp-chips` recipes (sysroot staging).
- Produces: a bitbake target `aen-a32-carrier-bringup` installed into the image.

- [ ] **Step 1: Write the recipe** (SRC_URI/SRCREV pattern from `meta-alp-sdk/recipes-core/alp-sdk/alp-sdk_0.6.bb`; branch override per the `pn-alp-sdk` validation in memory)

```bitbake
# SPDX-License-Identifier: Apache-2.0
#
# Bakes the aen-a32-carrier-bringup demo (examples/aen/) into the
# image.  Builds against the staged alp-sdk runtime + chip drivers;
# the rootfs only needs libalp_sdk.so (the chip lib links static).

SUMMARY = "E1M-AEN801 A32 Linux carrier bring-up example"
DESCRIPTION = "User-space bring-up smoke test for the E1M-EVK carrier \
               peripherals over the portable alp_* API (i2c-dev + \
               gpiochip): bus scan, tcal9538 I/O-expander, IMU chip-id, \
               and SoC GPIO."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=787726818c896f394f6627ab59d98d69"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=feat/aen-a32-yocto-bringup"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

# Build only the example subdir; it consumes the SDK from the sysroot.
S = "${WORKDIR}/git/examples/aen/aen-a32-carrier-bringup"

DEPENDS = "alp-sdk alp-chips"
RDEPENDS:${PN} = "alp-sdk"

inherit cmake

FILES:${PN} += "${bindir}/aen-a32-carrier-bringup"

# AEN A32 only -- the example is wired to the E1M-EVK on this machine.
COMPATIBLE_MACHINE = "e1m-aen801-a32"
```

- [ ] **Step 2: Add the image install line** to `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf`

Find the existing line (verified present at `:94`):
```
IMAGE_INSTALL:append = " alp-sdk"
```
Change it to:
```
IMAGE_INSTALL:append = " alp-sdk aen-a32-carrier-bringup"
```

- [ ] **Step 3: Confirm BBMASK does not catch the new dir**

Run:
```
MSYS_NO_PATHCONV=1 wsl bash -c "echo '/meta-alp-sdk/recipes-ros/ /meta-alp-sdk/recipes-deepx/ /meta-alp-sdk/recipes-images/' | grep -o 'recipes-examples' || echo 'recipes-examples NOT masked (good)'"
```
Expected: `recipes-examples NOT masked (good)`.

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-examples/aen-a32-carrier-bringup_0.6.bb meta-alp-sdk/conf/machine/e1m-aen801-a32.conf
git commit -m "yocto(aen): bake the A32 carrier bring-up example into alif-tiny-image"
```

---

### Task 3: Cross-build + bake validation (WSL, no board)

**Files:**
- Create: `C:\Users\caner\AppData\Local\Temp\claude\C--Users-caner-Documents-GitHub-alp-sdk\<session>\scratchpad\build_sp2.sh` (throwaway driver; not committed)

**Interfaces:**
- Consumes: Tasks 1+2 committed + pushed (the recipe fetches `${AUTOREV}` of the branch).
- Produces: a `cortexa32 musl` ELF in `tmp/work/...` and the example in the `alif-tiny-image` rootfs manifest.

- [ ] **Step 1: Push the branch** so the recipe's `${AUTOREV}` fetch sees the new sources

```bash
git push
```

- [ ] **Step 2: Write the bake driver** `scratchpad/build_sp2.sh` (resume recipe from memory; auto.conf AFTER the layer-add loop)

```bash
#!/usr/bin/env bash
set -euo pipefail
SETUP=~/alif-bsp-ref/alif_linux-apss-build-setup
export BITBAKEDIR=$SETUP/tools/bitbake
cd "$SETUP"
source layers/openembedded-core/oe-init-build-env build
# (layer-add loop already applied in the standing build/ dir; auto.conf
#  carries MACHINE=e1m-aen801-a32 + DISTRO=apss-tiny + the kernel/TFA vars
#  + BBMASK + BB_DANGLINGAPPENDS_WARNONLY per project memory.)
bitbake -c cleansstate aen-a32-carrier-bringup || true
bitbake aen-a32-carrier-bringup 2>&1 | tee /mnt/c/Users/caner/AppData/Local/Temp/claude/sp2_recipe.log
bitbake alif-tiny-image 2>&1 | tee /mnt/c/Users/caner/AppData/Local/Temp/claude/sp2_image.log
```

- [ ] **Step 3: Run it**

Run: `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/Users/caner/AppData/Local/Temp/claude/.../scratchpad/build_sp2.sh`
Expected: both bitbake invocations end with no ERROR lines.

- [ ] **Step 4: Assert the ELF arch**

Run (point at the built binary under tmp/work):
```
MSYS_NO_PATHCONV=1 wsl bash -c "cd ~/alif-bsp-ref/alif_linux-apss-build-setup/build && find tmp/work -name aen-a32-carrier-bringup -type f -path '*image*' -exec file {} \;"
```
Expected: `ELF 32-bit LSB ... ARM ... musl`.

- [ ] **Step 5: Assert it is in the rootfs manifest**

Run:
```
MSYS_NO_PATHCONV=1 wsl bash -c "grep -r aen-a32-carrier-bringup ~/alif-bsp-ref/alif_linux-apss-build-setup/build/tmp/deploy/images/e1m-aen801-a32/*.manifest"
```
Expected: a line naming the `aen-a32-carrier-bringup` package.

- [ ] **Step 6: Commit the evidence** (notes file only — scratchpad scripts stay uncommitted)

Append the bake result (recipe built, ELF arch, manifest line) to `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` under a new `## SP2 bake` heading, then:
```bash
git add docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md
git commit -m "docs(aen): SP2 example cross-builds + bakes into alif-tiny-image"
```

---

### Task 4: Docs + changelog

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `examples/aen/README.md` (add the new example to its index)
- Modify: `docs/bring-up-aen.md` (note the A32 Linux bring-up example, if the file has a peripherals/examples section — confirm with a read first)

**Interfaces:**
- Consumes: Tasks 1-3 complete.
- Produces: docs in sync (satisfies the post-commit doc-drift gate).

- [ ] **Step 1: Add a CHANGELOG entry** under the Unreleased/next section

```markdown
- **AEN A32 Linux:** new `examples/aen/aen-a32-carrier-bringup` — a user-space
  bring-up smoke test that drives the E1M-EVK carrier peripherals through the
  portable `alp_*` API over Linux i2c-dev + gpiochip (bus scan, TCAL9538
  I/O-expander, IMU chip-id, SoC GPIO). Baked into `alif-tiny-image` for
  `MACHINE=e1m-aen801-a32`.
```

- [ ] **Step 2: Add the example to `examples/aen/README.md`** (match the existing list format — read the file, append a row/bullet pointing at `aen-a32-carrier-bringup` and note it is the A32-Linux one, distinct from the M55/Zephyr regchecks).

- [ ] **Step 3: Run the doc-drift gate**

Run: `py -3.14 scripts/check_doc_drift.py`
Expected: exit 0 (no drift). Fix any flagged surface.

- [ ] **Step 4: Commit**

```bash
git add CHANGELOG.md examples/aen/README.md docs/bring-up-aen.md
git commit -m "docs(aen): document the A32 carrier bring-up example"
```

---

## Self-Review

**Spec coverage:**
- Example `examples/aen/aen-a32-carrier-bringup/` (i2c scan + tcal9538 + IMU + gpio) → Task 1. ✅
- `recipes-examples/` recipe + `IMAGE_INSTALL` → Task 2. ✅
- BBMASK non-collision check → Task 2 Step 3. ✅
- No-board validation (cross-compile + manifest assert) → Task 3. ✅
- Board-gated values isolated as `TODO(e1m-evk-hw)` → Task 1 main.c constants. ✅
- Docs/changelog → Task 4 (satisfies the doc-drift gate). ✅

**Placeholder scan:** No "TBD"/"implement later". The `TODO(e1m-evk-hw)` constants are the spec's deliberate board-gated markers, not plan gaps; their resolution procedure is in the README table.

**Type consistency:** `alp_gpio_configure` uses `ALP_GPIO_OUTPUT`/`ALP_GPIO_INPUT` (verified enum, not `*_DIR_*`). `alp_i2c_config_t` fields `bus_id`/`bitrate_hz` match the header. Chip init signatures `(ctx, alp_i2c_t*, addr)` match all three drivers. `EVK_I2C_ADDR_BMI323`=0x68, `EVK_I2C_ADDR_ICM42670`=0x69 per the board header. Recipe `${PN}` = `aen-a32-carrier-bringup` consistent across recipe + IMAGE_INSTALL + manifest grep.
