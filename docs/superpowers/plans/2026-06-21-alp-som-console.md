# Alp SoM Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a professional, Linux-like UART console to alp-sdk Zephyr firmware — an `alp` command tree on the Zephyr shell with portable board/gpio/i2c/adc/pwm/mem/clk/companion/reboot diagnostics — bench-validated on Alif AEN801, portable to V2N-M33.

**Architecture:** Build on the Zephyr shell subsystem (`CONFIG_SHELL`). One command root `alp` registered in `alp_console.c`; each command group lives in its own file and attaches with Zephyr's decentralized `SHELL_SUBCMD_ADD((alp), ...)`. Commands call the portable `<alp/*>` API. A two-tier Kconfig (`ALP_SDK_CONSOLE` read-only, `ALP_SDK_CONSOLE_UNSAFE` writes) plus per-group hardware auto-gating controls what compiles in.

**Tech Stack:** C (C11), Zephyr v4.4 shell + ztest, Kconfig, CMake (`zephyr_library_*`), native_sim for CI, clang-format-14.

## Global Constraints

Every task implicitly includes these (verbatim from the spec + repo rules):

- **Branch:** `feat/alp-som-console` worktree (already created, based on `feat/aen801-lcd-bringup`). Do not switch branches.
- **C house style:** clang-format-14, tabs, Consecutive alignment, BinPack off. Run the format gate before each commit (skill: `applying-the-alp-sdk-c-house-style`).
- **File header (every new `.c`/`.h`):**
  ```c
  /*
   * Copyright 2026 Alp Lab AB
   * SPDX-License-Identifier: Apache-2.0
   */
  ```
  No personal name/email. No `Co-Authored-By: Claude` in commits (skill: `feedback_no_claude_footer`, `feedback-attribution-alp-lab-ab`).
- **Branding:** "Alp", never "ALP".
- **Portable API only in app/example code:** examples use `<alp/*>`; `cc3501e_*` / `gd32g553_*` appear ONLY inside the console's internal companion shim (`alp_console_companion.c`), never in example `main.c`.
- **Status type:** `alp_status_t` (`ALP_OK == 0`, errors negative) from `<alp/peripheral.h>`.
- **Shell handler signature:** `static int cmd_x(const struct shell *sh, size_t argc, char **argv)`; return `0` on success, negative on error; emit user messages with `shell_print(sh, ...)` / `shell_error(sh, ...)` / `shell_warn(sh, ...)`.
- **Never fail a boot / never assert on user input.** Hardware-absent paths degrade with a clear message.
- **No placeholders, no silent caps:** if a verb is intentionally bench-only or deferred, say so in `--help` text and the docs.
- **Local CI before any push:** native_sim twister + format + ABI/check gates (skills: `running-local-ci`, `reference_local_twister_invocation`).

### Confirmed API reference (use these exact symbols)

```c
/* <alp/peripheral.h> */
typedef enum { ALP_GPIO_INPUT = 0, ALP_GPIO_OUTPUT = 1 } alp_gpio_dir_t;
typedef enum { ALP_GPIO_PULL_NONE=0, ALP_GPIO_PULL_UP=1, ALP_GPIO_PULL_DOWN=2 } alp_gpio_pull_t;
alp_gpio_t  *alp_gpio_open(uint32_t pin_id);
alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull);
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level);
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level);
void         alp_gpio_close(alp_gpio_t *pin);
typedef struct { uint32_t bus_id; uint32_t bitrate_hz; } alp_i2c_config_t;
alp_i2c_t   *alp_i2c_open(const alp_i2c_config_t *cfg);
alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len);
alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len);
alp_status_t alp_i2c_write_read(alp_i2c_t*, uint8_t addr, const uint8_t*, size_t, uint8_t*, size_t);
void         alp_i2c_close(alp_i2c_t *bus);
alp_status_t alp_last_error(void);

/* <alp/adc.h> */
typedef struct { uint32_t channel_id; uint8_t resolution_bits; /* .reference */ ... } alp_adc_config_t;
alp_adc_t   *alp_adc_open(const alp_adc_config_t *cfg);
alp_status_t alp_adc_read_raw(alp_adc_t *adc, int32_t *raw_out);
void         alp_adc_close(alp_adc_t *adc);

/* <alp/pwm.h> */
typedef struct { uint32_t channel_id; uint32_t period_ns; alp_pwm_polarity_t polarity; } alp_pwm_config_t;
alp_pwm_t   *alp_pwm_open(const alp_pwm_config_t *cfg);
alp_status_t alp_pwm_set_period(alp_pwm_t *pwm, uint32_t period_ns);
alp_status_t alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns);
void         alp_pwm_close(alp_pwm_t *pwm);

/* <alp/hw_info.h>  (only when CONFIG_ALP_SDK_HW_INFO) */
alp_status_t alp_hw_info_read(alp_hw_info_t *out);
/* fields: som_family, som_sku, som_hw_rev, som_serial, som_mfg_year/_month/_day */

/* companion (internal) */
/* V2N: src/zephyr/v2n_supervisor.h */
alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out);
void         alp_z_v2n_supervisor_release(void);
/* gd32g553_get_version(ctx,&ver) / gd32g553_ping(ctx) / gd32g553_gpio_read/write */
/* Alif: cc3501e_get_version(cc3501e_t*, uint16_t*) */
```

---

## File Structure

```
src/zephyr/console/alp_console.c            # root `alp` set + `alp board` + shared num parser
src/zephyr/console/alp_console_mem.c        # `alp mem rd|wr`
src/zephyr/console/alp_console_gpio.c       # `alp gpio read|mode|write`
src/zephyr/console/alp_console_i2c.c        # `alp i2c scan|read|write`
src/zephyr/console/alp_console_periph.c     # `alp adc read`, `alp pwm set`
src/zephyr/console/alp_console_sys.c        # `alp clk`, `alp reboot`
src/zephyr/console/alp_console_companion.c  # `alp companion` + V2N/Alif shim
src/zephyr/console/alp_console.h            # internal: number-parse helper proto, companion register API
include/alp/console.h                       # public: alp_console_companion_set() (Alif companion binding)
zephyr/Kconfig                              # +CONFIG_ALP_SDK_CONSOLE, +_UNSAFE, +_COMPANION
zephyr/CMakeLists.txt                       # zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE console/*.c)
tests/console/                              # native_sim ztest (dummy shell backend)
examples/peripheral-io/alp-console/         # teaching example + native_sim overlay
docs/console.md                             # usage doc
```

---

### Task 1: Console skeleton — Kconfig, CMake, root `alp` + `alp board`

**Files:**
- Create: `src/zephyr/console/alp_console.h`
- Create: `src/zephyr/console/alp_console.c`
- Modify: `zephyr/Kconfig` (add console configs inside the `if ALP_SDK` block, after `ALP_SDK_VERSION` at line ~31)
- Modify: `zephyr/CMakeLists.txt` (after the banner block at line ~66)
- Create: `tests/console/CMakeLists.txt`, `tests/console/prj.conf`, `tests/console/testcase.yaml`, `tests/console/src/test_console.c`

**Interfaces:**
- Produces: command root `alp` (Zephyr `SHELL_SUBCMD_SET_CREATE(alp_subcmds, (alp))` + `SHELL_CMD_REGISTER(alp, &alp_subcmds, ...)`). Later tasks attach groups with `SHELL_SUBCMD_ADD((alp), <group>, ...)`.
- Produces: `int alp_console_parse_ulong(const char *s, unsigned long *out)` (shared hex/dec parser) declared in `alp_console.h`.

- [ ] **Step 1: Write the failing test**

`tests/console/src/test_console.c`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <string.h>

/* Run a shell line on the dummy backend and return its captured output. */
static const char *run(const char *line)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, line);

	size_t      len;
	const char *out = shell_backend_dummy_get_output(sh, &len);
	return out;
}

ZTEST(alp_console, test_board_reports_version)
{
	const char *out = run("alp board");

	zassert_not_null(strstr(out, "Alp SDK"), "banner line missing: %s", out);
	zassert_not_null(strstr(out, CONFIG_ALP_SDK_VERSION), "version missing");
}

ZTEST_SUITE(alp_console, NULL, NULL, NULL, NULL, NULL);
```

`tests/console/prj.conf`:
```
CONFIG_ZTEST=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SDK_CONSOLE=y
CONFIG_ALP_SDK_CONSOLE_UNSAFE=y
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=n
CONFIG_SHELL_BACKEND_DUMMY=y
CONFIG_LOG=n
```

`tests/console/testcase.yaml`:
```yaml
tests:
  alp_sdk.console:
    platform_allow: native_sim native_sim/native/64
    tags: alp_sdk console
```

`tests/console/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_console_test)
target_sources(app PRIVATE src/test_console.c)
```

- [ ] **Step 2: Run the test to verify it fails**

Run (WSL, per `reference_local_twister_invocation`):
```
west twister -p native_sim -T tests/console --testsuite-root tests/console \
  -O /tmp/tw-console -v
```
Expected: BUILD FAIL — `CONFIG_ALP_SDK_CONSOLE` is an unknown symbol / no `alp` command.

- [ ] **Step 3: Add the Kconfig symbols**

In `zephyr/Kconfig`, immediately after the `ALP_SDK_VERSION` block (line ~31, still inside `if ALP_SDK`):
```
config ALP_SDK_CONSOLE
	bool "Alp SoM interactive console (Zephyr shell + `alp` commands)"
	depends on ALP_SDK
	select SHELL
	help
	  Register the `alp` command tree on the Zephyr shell: board
	  identity, gpio / i2c / adc / pwm pokes via the portable <alp/*>
	  API, a memory peek, a clock dump, and a companion-chip bridge.
	  Read-only verbs only unless ALP_SDK_CONSOLE_UNSAFE is also set.

config ALP_SDK_CONSOLE_UNSAFE
	bool "Allow write / destructive `alp` console commands"
	depends on ALP_SDK_CONSOLE
	default n
	help
	  Compile the write / destructive verbs into the console:
	  `mem wr`, `gpio write`, `gpio mode`, `i2c write`, `pwm set`,
	  companion writes, and `reboot`.  Leave OFF for field / production
	  builds so a mis-typed poke cannot brick a live unit; the verbs are
	  then physically absent from the binary and from `help`.
```

- [ ] **Step 4: Wire CMake**

In `zephyr/CMakeLists.txt`, after the banner block (line ~66):
```cmake
# Alp SoM console (CONFIG_ALP_SDK_CONSOLE) -- the `alp` Zephyr-shell command
# tree.  Each group is its own TU and attaches to the `alp` root via the
# decentralized SHELL_SUBCMD_ADD section mechanism, so they list here flat.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console.c)
```
(Later tasks append their files to this `ifdef` block.)

- [ ] **Step 5: Write the header**

`src/zephyr/console/alp_console.h`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal shared helpers for the Alp SoM console command groups.
 */
#ifndef ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_
#define ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_

#include <stdbool.h>

/**
 * @brief Parse a decimal or 0x-hex unsigned integer from a shell arg.
 * @return 0 on success, -EINVAL on a malformed / out-of-range token.
 */
int alp_console_parse_ulong(const char *s, unsigned long *out);

#endif /* ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_ */
```

- [ ] **Step 6: Write the root command + `alp board`**

`src/zephyr/console/alp_console.c`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- the `alp` Zephyr-shell command root, the shared
 * argument parser, and the `alp board` identity command.  Mirrors the
 * boot banner (src/zephyr/alp_banner.c): same SDK version + EEPROM
 * identity source, printed on demand.
 */
#include <errno.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "alp_console.h"

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h>
#endif

int alp_console_parse_ulong(const char *s, unsigned long *out)
{
	if (s == NULL || *s == '\0') {
		return -EINVAL;
	}

	char         *end = NULL;
	unsigned long v   = strtoul(s, &end, 0); /* base 0: 0x.. hex, else dec */

	if (end == s || *end != '\0') {
		return -EINVAL;
	}
	*out = v;
	return 0;
}

static int cmd_board(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;

	if (alp_hw_info_read(&info) == ALP_OK && info.som_sku[0] != '\0') {
		shell_print(sh, "Alp SDK %s  |  %s %s  |  (c) Alp Lab AB",
			    CONFIG_ALP_SDK_VERSION, info.som_sku, info.som_hw_rev);
		shell_print(sh, "  family : %s", info.som_family);
		shell_print(sh, "  serial : %s", info.som_serial);
		shell_print(sh, "  mfg    : %04u-%02u-%02u", info.som_mfg_year,
			    info.som_mfg_month, info.som_mfg_day);
	} else
#endif
	{
		shell_print(sh, "Alp SDK %s  |  %s  |  (c) Alp Lab AB",
			    CONFIG_ALP_SDK_VERSION, CONFIG_BOARD);
	}

	shell_print(sh, "  uptime : %llu ms", (unsigned long long)k_uptime_get());
	return 0;
}

SHELL_SUBCMD_SET_CREATE(alp_subcmds, (alp));

SHELL_SUBCMD_ADD((alp), board, NULL, "SoM identity, SDK version, uptime", cmd_board, 1, 0);

SHELL_CMD_REGISTER(alp, &alp_subcmds, "Alp SoM diagnostics console", NULL);
```

- [ ] **Step 7: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS (`alp_sdk.console` 1/1).

- [ ] **Step 8: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console.c src/zephyr/console/alp_console.h tests/console/src/test_console.c
git add src/zephyr/console zephyr/Kconfig zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp shell root + \`alp board\` identity command"
```

---

### Task 2: `alp mem rd|wr` (write unsafe-gated)

**Files:**
- Create: `src/zephyr/console/alp_console_mem.c`
- Modify: `zephyr/CMakeLists.txt` (append to the console `ifdef` block)
- Modify: `tests/console/src/test_console.c` (add cases)

**Interfaces:**
- Consumes: `alp_console_parse_ulong()` (Task 1); the `(alp)` root set.
- Produces: `alp mem` subgroup.

- [ ] **Step 1: Write the failing test** — append to `test_console.c`:
```c
ZTEST(alp_console, test_mem_rd_reads_known_word)
{
	static volatile uint32_t probe = 0xCAFEF00Du;
	char                     line[48];

	snprintk(line, sizeof(line), "alp mem rd 0x%lx", (unsigned long)(uintptr_t)&probe);
	const char *out = run(line);

	zassert_not_null(strstr(out, "cafef00d"), "expected value in: %s", out);
}

ZTEST(alp_console, test_mem_wr_then_rd_roundtrips)
{
	static volatile uint32_t probe = 0;
	char                     line[64];

	snprintk(line, sizeof(line), "alp mem wr 0x%lx 0x12345678",
		 (unsigned long)(uintptr_t)&probe);
	(void)run(line);
	zassert_equal(probe, 0x12345678u, "write did not land");
}
```

- [ ] **Step 2: Run to verify it fails**
```
west twister -p native_sim -T tests/console --testsuite-root tests/console -O /tmp/tw-console -c
```
Expected: `alp mem` unknown command → test FAIL.

- [ ] **Step 3: Implement `alp_console_mem.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp mem` -- width-aware volatile memory peek (rd) and poke (wr).
 * rd is the always-available register-debug primitive; wr is gated on
 * CONFIG_ALP_SDK_CONSOLE_UNSAFE.
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "alp_console.h"

static int cmd_mem_rd(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long addr;
	unsigned long count = 1;

	if (alp_console_parse_ulong(argv[1], &addr) != 0) {
		shell_error(sh, "bad address");
		return -EINVAL;
	}
	if (argc == 3 && alp_console_parse_ulong(argv[2], &count) != 0) {
		shell_error(sh, "bad count");
		return -EINVAL;
	}

	for (unsigned long i = 0; i < count; i++) {
		uintptr_t          a = (uintptr_t)addr + i * sizeof(uint32_t);
		volatile uint32_t *p = (volatile uint32_t *)a;

		shell_print(sh, "[%08lx] = %08x", (unsigned long)a, *p);
	}
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_mem_wr(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long addr;
	unsigned long val;

	if (alp_console_parse_ulong(argv[1], &addr) != 0 ||
	    alp_console_parse_ulong(argv[2], &val) != 0) {
		shell_error(sh, "usage: alp mem wr <addr> <u32>");
		return -EINVAL;
	}

	*(volatile uint32_t *)(uintptr_t)addr = (uint32_t)val;
	shell_print(sh, "[%08lx] <- %08lx", addr, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_mem_subcmds,
	SHELL_CMD_ARG(rd, NULL, "rd <addr> [words] -- read u32(s)", cmd_mem_rd, 2, 1),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
	SHELL_CMD_ARG(wr, NULL, "wr <addr> <u32> -- write u32 (UNSAFE)", cmd_mem_wr, 3, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), mem, &alp_mem_subcmds, "Memory / register peek-poke", NULL, 1, 0);
```

- [ ] **Step 4: Append to CMake** — extend the console `ifdef` block in `zephyr/CMakeLists.txt`:
```cmake
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_mem.c)
```

- [ ] **Step 5: Run to verify it passes** — Step 2 command. Expected: PASS (3/3).

- [ ] **Step 6: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_mem.c tests/console/src/test_console.c
git add src/zephyr/console/alp_console_mem.c zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp mem rd/wr peek-poke (wr unsafe-gated)"
```

---

### Task 3: `alp gpio read|mode|write` (mode/write unsafe-gated)

**Files:**
- Create: `src/zephyr/console/alp_console_gpio.c`
- Modify: `zephyr/CMakeLists.txt`, `tests/console/src/test_console.c`, `tests/console/prj.conf`

**Interfaces:**
- Consumes: `alp_console_parse_ulong()`; portable `alp_gpio_*`. Pins are passed as numeric `pin_id` (the `E1M_*` indices are plain integers).
- Produces: `alp gpio` subgroup.

- [ ] **Step 1: Enable a GPIO the native_sim test can drive** — append to `tests/console/prj.conf`:
```
CONFIG_GPIO=y
```
and add a `tests/console/app.overlay` exposing the native_sim gpio emulator as `alp-gpio` index 0 (the SDK gpio backend resolves `pin_id` through the board's pin map; on native_sim the test drives pin 0 of `gpio0`):
```dts
/ {
	aliases { alp-gpio0 = &gpio0; };
};
```

- [ ] **Step 2: Write the failing test** — append:
```c
ZTEST(alp_console, test_gpio_read_runs)
{
	const char *out = run("alp gpio read 0");

	/* On native_sim pin 0 reads back a defined level; just assert the
	 * command resolved and printed a 0/1, not an error. */
	zassert_true(strstr(out, "= 0") || strstr(out, "= 1"), "got: %s", out);
}
```

- [ ] **Step 3: Run to verify it fails** — Step 2 of Task 2's run command. Expected: `alp gpio` unknown → FAIL.

- [ ] **Step 4: Implement `alp_console_gpio.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp gpio` -- read / configure / write a portable GPIO by pin_id
 * (the E1M_* / BOARD_* indices from <alp/e1m_pinout.h>).
 */
#include <errno.h>

#include <zephyr/shell/shell.h>

#include <alp/peripheral.h>

#include "alp_console.h"

static int cmd_gpio_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;

	if (alp_console_parse_ulong(argv[1], &pin) != 0) {
		shell_error(sh, "bad pin id");
		return -EINVAL;
	}

	alp_gpio_t *g = alp_gpio_open((uint32_t)pin);

	if (g == NULL) {
		shell_error(sh, "open pin %lu failed (err %d)", pin, (int)alp_last_error());
		return -EIO;
	}

	(void)alp_gpio_configure(g, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);

	bool         level = false;
	alp_status_t s     = alp_gpio_read(g, &level);

	alp_gpio_close(g);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pin %lu = %d", pin, (int)level);
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_gpio_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;
	unsigned long val;

	if (alp_console_parse_ulong(argv[1], &pin) != 0 ||
	    alp_console_parse_ulong(argv[2], &val) != 0 || val > 1) {
		shell_error(sh, "usage: alp gpio write <pin> <0|1>");
		return -EINVAL;
	}

	alp_gpio_t *g = alp_gpio_open((uint32_t)pin);

	if (g == NULL) {
		shell_error(sh, "open pin %lu failed (err %d)", pin, (int)alp_last_error());
		return -EIO;
	}
	(void)alp_gpio_configure(g, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

	alp_status_t s = alp_gpio_write(g, val != 0);

	alp_gpio_close(g);
	if (s != ALP_OK) {
		shell_error(sh, "write failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pin %lu <- %lu", pin, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_gpio_subcmds,
	SHELL_CMD_ARG(read, NULL, "read <pin> -- sample a pin as input", cmd_gpio_read, 2, 0),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
	SHELL_CMD_ARG(write, NULL, "write <pin> <0|1> -- drive a pin (UNSAFE)", cmd_gpio_write, 3, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), gpio, &alp_gpio_subcmds, "GPIO read / write by pin_id", NULL, 1, 0);
```

- [ ] **Step 5: Append to CMake** — add `alp_console_gpio.c` to the console `ifdef` block.

- [ ] **Step 6: Run to verify it passes.** Expected: PASS.

- [ ] **Step 7: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_gpio.c tests/console/src/test_console.c
git add src/zephyr/console/alp_console_gpio.c zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp gpio read/write by portable pin_id (write unsafe)"
```

---

### Task 4: `alp i2c scan|read|write` (write unsafe-gated)

**Files:**
- Create: `src/zephyr/console/alp_console_i2c.c`
- Modify: `zephyr/CMakeLists.txt`, `tests/console/src/test_console.c`, `tests/console/prj.conf`

**Interfaces:**
- Consumes: `alp_console_parse_ulong()`; portable `alp_i2c_*`. Bus passed as numeric `bus_id`. Scan idiom = 1-byte read on 0x08..0x77 (matches `examples/peripheral-io/i2c-scanner`).
- Produces: `alp i2c` subgroup.

- [ ] **Step 1: Enable the I2C sw-fallback for the test** — append to `tests/console/prj.conf`:
```
CONFIG_I2C=y
CONFIG_ALP_SDK_I2C_SW_FALLBACK=y
```

- [ ] **Step 2: Write the failing test** — append:
```c
ZTEST(alp_console, test_i2c_scan_runs)
{
	const char *out = run("alp i2c scan 0");

	zassert_not_null(strstr(out, "responder"), "scan summary missing: %s", out);
}
```

- [ ] **Step 3: Run to verify it fails.** Expected: `alp i2c` unknown → FAIL.

- [ ] **Step 4: Implement `alp_console_i2c.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp i2c` -- scan a bus, read / write a register, by portable bus_id.
 * Probe idiom: a 1-byte read on each 7-bit address (the portable probe
 * -- some controllers put nothing on the bus for a zero-length write).
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <alp/peripheral.h>

#include "alp_console.h"

static alp_i2c_t *open_bus(const struct shell *sh, const char *arg)
{
	unsigned long bus;

	if (alp_console_parse_ulong(arg, &bus) != 0) {
		shell_error(sh, "bad bus id");
		return NULL;
	}

	alp_i2c_t *h = alp_i2c_open(&(alp_i2c_config_t){
		.bus_id     = (uint32_t)bus,
		.bitrate_hz = 100000,
	});

	if (h == NULL) {
		shell_error(sh, "open bus %lu failed (err %d)", bus, (int)alp_last_error());
	}
	return h;
}

static int cmd_i2c_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	int found = 0;

	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t scratch;

		if (alp_i2c_read(bus, addr, &scratch, 1) == ALP_OK) {
			shell_print(sh, "  0x%02x", addr);
			found++;
		}
	}
	alp_i2c_close(bus);
	shell_print(sh, "scan complete, %d responder(s)", found);
	return 0;
}

static int cmd_i2c_read(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long addr;
	unsigned long reg;
	unsigned long len = 1;

	if (alp_console_parse_ulong(argv[2], &addr) != 0 ||
	    alp_console_parse_ulong(argv[3], &reg) != 0 ||
	    (argc == 5 && alp_console_parse_ulong(argv[4], &len) != 0) || len == 0 || len > 16) {
		shell_error(sh, "usage: alp i2c read <bus> <addr> <reg> [len<=16]");
		return -EINVAL;
	}

	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	uint8_t      r = (uint8_t)reg;
	uint8_t      buf[16];
	alp_status_t s = alp_i2c_write_read(bus, (uint8_t)addr, &r, 1, buf, (size_t)len);

	alp_i2c_close(bus);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	for (unsigned long i = 0; i < len; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x ", buf[i]);
	}
	shell_print(sh, "");
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_i2c_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long addr;
	unsigned long reg;
	unsigned long val;

	if (alp_console_parse_ulong(argv[2], &addr) != 0 ||
	    alp_console_parse_ulong(argv[3], &reg) != 0 ||
	    alp_console_parse_ulong(argv[4], &val) != 0) {
		shell_error(sh, "usage: alp i2c write <bus> <addr> <reg> <u8>");
		return -EINVAL;
	}

	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	uint8_t      payload[2] = {(uint8_t)reg, (uint8_t)val};
	alp_status_t s          = alp_i2c_write(bus, (uint8_t)addr, payload, sizeof(payload));

	alp_i2c_close(bus);
	if (s != ALP_OK) {
		shell_error(sh, "write failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "0x%02lx[0x%02lx] <- 0x%02lx", addr, reg, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_i2c_subcmds,
	SHELL_CMD_ARG(scan, NULL, "scan <bus>", cmd_i2c_scan, 2, 0),
	SHELL_CMD_ARG(read, NULL, "read <bus> <addr> <reg> [len]", cmd_i2c_read, 4, 1),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
	SHELL_CMD_ARG(write, NULL, "write <bus> <addr> <reg> <u8> (UNSAFE)", cmd_i2c_write, 5, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), i2c, &alp_i2c_subcmds, "I2C scan / register read-write", NULL, 1, 0);
```

- [ ] **Step 5: Append to CMake.** Add `alp_console_i2c.c`.

- [ ] **Step 6: Run to verify it passes.** Expected: PASS.

- [ ] **Step 7: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_i2c.c tests/console/src/test_console.c
git add src/zephyr/console/alp_console_i2c.c zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp i2c scan/read/write by portable bus_id (write unsafe)"
```

---

### Task 5: `alp adc read` + `alp pwm set` (pwm unsafe-gated)

**Files:**
- Create: `src/zephyr/console/alp_console_periph.c`
- Modify: `zephyr/CMakeLists.txt`, `tests/console/src/test_console.c`, `tests/console/prj.conf`

**Interfaces:**
- Consumes: `alp_console_parse_ulong()`; portable `alp_adc_*`, `alp_pwm_*`.
- Produces: `alp adc`, `alp pwm` subgroups.
- Note: PWM has no portable duty-read in `<alp/pwm.h>`, so v1 exposes only `pwm set <ch> <period_ns> <duty_ns>` (no `get`); documented in `--help`.

- [ ] **Step 1: Enable ADC sw-fallback for the test** — append to `tests/console/prj.conf`:
```
CONFIG_ADC=y
CONFIG_ALP_SDK_ADC_SW_FALLBACK=y
```
(If no ADC sw-fallback Kconfig exists, gate the ADC test case behind `#if DT_HAS_ALIAS(alp_adc0)` and rely on the bench for ADC; see Step 4 note. Verify the symbol with `git grep ALP_SDK_ADC_SW_FALLBACK` before relying on it.)

- [ ] **Step 2: Write the failing test** — append (registration smoke; ADC value depends on backend):
```c
ZTEST(alp_console, test_adc_read_registers)
{
	const char *out = run("alp adc read 0");

	/* Either a raw value or a clean "open failed" message -- never a crash
	 * and never "command not found". */
	zassert_is_null(strstr(out, "command not found"), "adc cmd missing: %s", out);
}
```

- [ ] **Step 3: Run to verify it fails.** Expected: `alp adc` unknown → FAIL.

- [ ] **Step 4: Implement `alp_console_periph.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp adc` (read a channel raw) + `alp pwm` (set period/duty).  Both
 * speak the portable <alp/adc.h> / <alp/pwm.h> surface by channel_id.
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <alp/adc.h>
#include <alp/pwm.h>

#include "alp_console.h"

static int cmd_adc_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long ch;

	if (alp_console_parse_ulong(argv[1], &ch) != 0) {
		shell_error(sh, "bad channel");
		return -EINVAL;
	}

	alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
		.channel_id      = (uint32_t)ch,
		.resolution_bits = 12,
	});

	if (adc == NULL) {
		shell_error(sh, "open ch %lu failed (err %d)", ch, (int)alp_last_error());
		return -EIO;
	}

	int32_t      raw = 0;
	alp_status_t s   = alp_adc_read_raw(adc, &raw);

	alp_adc_close(adc);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "adc[%lu] raw = %d", ch, raw);
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_pwm_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long ch;
	unsigned long period_ns;
	unsigned long duty_ns;

	if (alp_console_parse_ulong(argv[1], &ch) != 0 ||
	    alp_console_parse_ulong(argv[2], &period_ns) != 0 ||
	    alp_console_parse_ulong(argv[3], &duty_ns) != 0 || duty_ns > period_ns) {
		shell_error(sh, "usage: alp pwm set <ch> <period_ns> <duty_ns<=period>");
		return -EINVAL;
	}

	alp_pwm_t *pwm = alp_pwm_open(&(alp_pwm_config_t){
		.channel_id = (uint32_t)ch,
		.period_ns  = (uint32_t)period_ns,
	});

	if (pwm == NULL) {
		shell_error(sh, "open ch %lu failed (err %d)", ch, (int)alp_last_error());
		return -EIO;
	}

	alp_status_t s = alp_pwm_set_period(pwm, (uint32_t)period_ns);

	if (s == ALP_OK) {
		s = alp_pwm_set_duty(pwm, (uint32_t)duty_ns);
	}
	alp_pwm_close(pwm);
	if (s != ALP_OK) {
		shell_error(sh, "set failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pwm[%lu] period=%luns duty=%luns", ch, period_ns, duty_ns);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_adc_subcmds,
	SHELL_CMD_ARG(read, NULL, "read <ch> -- one-shot raw conversion", cmd_adc_read, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((alp), adc, &alp_adc_subcmds, "ADC one-shot read", NULL, 1, 0);

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_pwm_subcmds,
	SHELL_CMD_ARG(set, NULL, "set <ch> <period_ns> <duty_ns> (UNSAFE)", cmd_pwm_set, 4, 0),
	SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((alp), pwm, &alp_pwm_subcmds, "PWM set period/duty (UNSAFE)", NULL, 1, 0);
#endif
```

- [ ] **Step 5: Append to CMake.** Add `alp_console_periph.c`.

- [ ] **Step 6: Run to verify it passes.** Expected: PASS.

- [ ] **Step 7: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_periph.c tests/console/src/test_console.c
git add src/zephyr/console/alp_console_periph.c zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp adc read + alp pwm set (pwm unsafe-gated)"
```

---

### Task 6: `alp clk` (dump) + `alp reboot` (unsafe-gated)

**Files:**
- Create: `src/zephyr/console/alp_console_sys.c`
- Modify: `zephyr/CMakeLists.txt`, `tests/console/src/test_console.c`

**Interfaces:**
- Produces: `alp clk`, `alp reboot` subgroups.
- `alp clk` v1 = system clock facts (`sys_clock_hw_cycles_per_sec()`, ticks/sec). A full per-IP clock-tree dump is SoC-specific and a documented follow-up.
- `alp reboot` uses `sys_reboot()` (`<zephyr/sys/reboot.h>`, needs `CONFIG_REBOOT`); compiled only under UNSAFE.

- [ ] **Step 1: Write the failing test** — append:
```c
ZTEST(alp_console, test_clk_dump_runs)
{
	const char *out = run("alp clk");

	zassert_not_null(strstr(out, "Hz"), "clk dump missing: %s", out);
}
```

- [ ] **Step 2: Run to verify it fails.** Expected: `alp clk` unknown → FAIL.

- [ ] **Step 3: Implement `alp_console_sys.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp clk` (system-clock facts) and `alp reboot` (warm reset, UNSAFE).
 */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE) && IS_ENABLED(CONFIG_REBOOT)
#include <zephyr/sys/reboot.h>
#endif

static int cmd_clk(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "sys clock : %u Hz", (unsigned int)sys_clock_hw_cycles_per_sec());
	shell_print(sh, "tick rate : %d Hz", (int)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	return 0;
}

SHELL_SUBCMD_ADD((alp), clk, NULL, "System clock facts", cmd_clk, 1, 0);

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_REBOOT)
	shell_print(sh, "rebooting...");
	sys_reboot(SYS_REBOOT_WARM);
	return 0; /* not reached */
#else
	shell_error(sh, "CONFIG_REBOOT not enabled in this build");
	return -ENOTSUP;
#endif
}

SHELL_SUBCMD_ADD((alp), reboot, NULL, "Warm reset (UNSAFE)", cmd_reboot, 1, 0);
#endif
```

- [ ] **Step 4: Append to CMake.** Add `alp_console_sys.c`.

- [ ] **Step 5: Run to verify it passes.** Expected: PASS. (The test runs `alp clk` only — it never invokes `alp reboot`, which would reset native_sim.)

- [ ] **Step 6: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_sys.c tests/console/src/test_console.c
git add src/zephyr/console/alp_console_sys.c zephyr/CMakeLists.txt tests/console
git commit -m "feat(console): alp clk dump + alp reboot (unsafe-gated)"
```

---

### Task 7: `alp companion` — portable V2N/Alif companion bridge

**Files:**
- Create: `include/alp/console.h` (public Alif registration API)
- Create: `src/zephyr/console/alp_console_companion.c`
- Modify: `zephyr/Kconfig` (add `ALP_SDK_CONSOLE_COMPANION`), `zephyr/CMakeLists.txt`

**Interfaces:**
- Produces (public): `void alp_console_companion_set(cc3501e_t *ctx);` — the Alif app calls this once after `cc3501e_init()` to bind the companion the console talks to.
- Consumes: V2N `alp_z_v2n_supervisor_acquire/release` + `gd32g553_get_version/ping/gpio_read/gpio_write`; Alif `cc3501e_get_version`.
- Auto-gate: `ALP_SDK_CONSOLE_COMPANION` defaults on when a companion is configured.
- Scope note: v1 portable verbs = `ver`, `ping`. `gpio read/write` are V2N-only (the GD32 exposes a portable register; CC3501E does not). `ota status` is a documented follow-up (the two OTA APIs differ). State this in `--help` and `docs/console.md` — no silent cap.

- [ ] **Step 1: Add the auto-gate Kconfig** — in `zephyr/Kconfig` after `ALP_SDK_CONSOLE_UNSAFE`:
```
config ALP_SDK_CONSOLE_COMPANION
	bool "Add `alp companion` (GD32 on V2N / CC3501E on Alif)"
	depends on ALP_SDK_CONSOLE
	default y if ALP_SDK_V2N_SUPERVISOR || ALP_SDK_CHIP_CC3501E
	default n
	help
	  Compile the `alp companion` command group.  On V2N it binds the
	  GD32 supervisor singleton automatically; on Alif the application
	  registers its CC3501E handle via alp_console_companion_set().
```
(Verify the CC3501E chip Kconfig symbol name with `git grep "config ALP_SDK_CHIP_CC3501E"`; if it differs, use the real one.)

- [ ] **Step 2: Public header** — `include/alp/console.h`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- application-facing hooks.
 */
#ifndef ALP_CONSOLE_H_
#define ALP_CONSOLE_H_

#include <alp/chips/cc3501e.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the CC3501E companion the `alp companion` command talks to.
 *
 * On Alif there is no companion singleton, so the application opens its
 * CC3501E (cc3501e_init) and registers the handle here once.  No-op on
 * SoMs whose companion is a singleton (V2N binds the GD32 supervisor
 * automatically).  Pass NULL to unbind.
 *
 * @param ctx  Initialised CC3501E context, or NULL.
 */
void alp_console_companion_set(cc3501e_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CONSOLE_H_ */
```

- [ ] **Step 3: Implement `alp_console_companion.c`**
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion` -- one portable command surface over two different
 * companions: the GD32 supervisor singleton on V2N, an app-registered
 * CC3501E on Alif.  ver/ping are portable; gpio is V2N-only.
 */
#include <errno.h>

#include <zephyr/shell/shell.h>

#include <alp/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#include "../v2n_supervisor.h"
#endif

/* ---- Alif: app-registered CC3501E handle ---------------------------- */
static cc3501e_t *companion_cc3501e;

void alp_console_companion_set(cc3501e_t *ctx)
{
	companion_cc3501e = ctx;
}

static int cmd_companion_ver(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	gd32g553_version_t v;

	s = gd32g553_get_version(ctx, &v);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "GD32 supervisor fw v%u.%u.%u", v.major, v.minor, v.patch);
	return 0;
#else
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered (call alp_console_companion_set)");
		return -ENODEV;
	}

	uint16_t     ver = 0;
	alp_status_t s   = cc3501e_get_version(companion_cc3501e, &ver);

	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "CC3501E protocol v%u", (unsigned int)ver);
	return 0;
#endif
}

static int cmd_companion_ping(const struct shell *sh, size_t argc, char **argv)
{
#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}
	s = gd32g553_ping(ctx);
	alp_z_v2n_supervisor_release();
	shell_print(sh, "ping %s", s == ALP_OK ? "OK" : "FAIL");
	return s == ALP_OK ? 0 : -EIO;
#else
	/* CC3501E has no bare PING wrapper; a GET_VERSION round-trip is the
	 * liveness probe (this is how the on-silicon bring-up proved the link). */
	return cmd_companion_ver(sh, argc, argv);
#endif
}

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
static int cmd_companion_gpio_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;

	if (alp_console_parse_ulong(argv[1], &pin) != 0 || pin > 31) {
		shell_error(sh, "usage: alp companion gpio read <0..31>");
		return -EINVAL;
	}

	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	uint32_t levels = 0;

	s = gd32g553_gpio_read(ctx, BIT(pin), &levels);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "gpio_read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "companion pin %lu = %d", pin, (levels & BIT(pin)) ? 1 : 0);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_companion_gpio_subcmds,
	SHELL_CMD_ARG(read, NULL, "read <0..31>", cmd_companion_gpio_read, 2, 0),
	SHELL_SUBCMD_SET_END);
#endif /* V2N */

SHELL_STATIC_SUBCMD_SET_CREATE(
	alp_companion_subcmds,
	SHELL_CMD_ARG(ver, NULL, "companion firmware version", cmd_companion_ver, 1, 0),
	SHELL_CMD_ARG(ping, NULL, "liveness round-trip", cmd_companion_ping, 1, 0),
#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	SHELL_CMD(gpio, &alp_companion_gpio_subcmds, "companion GPIO (V2N)", NULL),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), companion, &alp_companion_subcmds,
		 "Companion chip bridge (GD32 / CC3501E)", NULL, 1, 0);
```

- [ ] **Step 4: Wire CMake** — gate this file on the companion sub-config:
```cmake
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE_COMPANION
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_companion.c)
```

- [ ] **Step 5: Build-verify both companion paths**

The companion group is not exercised on native_sim (no companion there). Build-check both arms compile:
```
# V2N arm (compile only): an existing V2N-M33 build with the console on
west build -p -b <v2n_m33_board> examples/peripheral-io/alp-console \
  -- -DCONFIG_ALP_SDK_CONSOLE=y -DCONFIG_ALP_SDK_CONSOLE_UNSAFE=y
```
Expected: links clean (the GD32 arm of `alp_console_companion.c` compiles). The Alif arm is covered by the example build in Task 8.

- [ ] **Step 6: Format + commit**
```bash
clang-format -i src/zephyr/console/alp_console_companion.c include/alp/console.h
git add include/alp/console.h src/zephyr/console/alp_console_companion.c zephyr/Kconfig zephyr/CMakeLists.txt
git commit -m "feat(console): alp companion bridge (GD32 on V2N, CC3501E on Alif)"
```

---

### Task 8: AEN801 example app + native_sim coverage + docs

**Files:**
- Create: `examples/peripheral-io/alp-console/src/main.c`
- Create: `examples/peripheral-io/alp-console/CMakeLists.txt`
- Create: `examples/peripheral-io/alp-console/prj.conf`
- Create: `examples/peripheral-io/alp-console/board.yaml`
- Create: `examples/peripheral-io/alp-console/boards/native_sim_native_64.overlay`
- Create: `examples/peripheral-io/alp-console/generated/alp.conf` + `native_sim.conf` (per `native_sim.conf` overlay convention)
- Create: `docs/console.md`
- Modify: `examples/README` index if one exists (`git grep -l "peripheral-io" -- '*README*'`)

**Interfaces:**
- Consumes: everything above; on Alif, `alp_console_companion_set()` from `<alp/console.h>`.

- [ ] **Step 1: Write the teaching example** — `examples/peripheral-io/alp-console/src/main.c` (~50% comment ratio per `feedback_examples_are_documentation`):
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-console -- bring up the Alp SoM interactive console.
 *
 * The console itself is SDK infrastructure: enabling CONFIG_ALP_SDK_CONSOLE
 * registers the whole `alp` command tree on the Zephyr shell at link time,
 * so an app gets board / gpio / i2c / adc / pwm / mem / clk diagnostics for
 * free -- main() does NOT register any commands.
 *
 * The ONE thing an app does here is bind the companion chip so
 * `alp companion` can reach it.  On V2N that is automatic (the GD32
 * supervisor is a singleton).  On Alif the CC3501E has no singleton, so we
 * open it and hand the handle to the console.  On boards without a CC3501E
 * this whole block compiles out.
 */
#include <zephyr/kernel.h>

#if defined(CONFIG_ALP_SDK_CHIP_CC3501E)
#include <alp/console.h>
#include <alp/chips/cc3501e.h>
#include <alp/spi.h>
#include <alp/board.h>

/* Storage for the companion context the console borrows for its lifetime. */
static cc3501e_t companion;

static void bind_companion(void)
{
	/* Open the SPI link the CC3501E sits on (board-defined alias) and
	 * run the init handshake; register the handle so `alp companion`
	 * can query it.  Best-effort: a missing companion just leaves the
	 * command reporting "not registered". */
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
		.bus_id = BOARD_SPI_CC3501E,
	});

	if (spi != NULL && cc3501e_init(&companion, spi) == ALP_OK) {
		alp_console_companion_set(&companion);
	}
}
#else
static void bind_companion(void)
{
}
#endif

int main(void)
{
	/* The boot banner (src/zephyr/alp_banner.c) has already printed; the
	 * shell prompt follows.  Type `alp` then Tab to explore. */
	bind_companion();
	return 0;
}
```
(Verify `BOARD_SPI_CC3501E` / `alp_spi_open` / `alp_spi_config_t` names with `git grep` against `<alp/spi.h>` and `<alp/board.h>`; substitute the real alias the AEN801 board.yaml exposes. If the CC3501E bring-up example already has a canonical open snippet, copy it verbatim.)

- [ ] **Step 2: prj.conf (bench build, both tiers + serial shell)**
```
CONFIG_ALP_SDK=y
CONFIG_ALP_SDK_CONSOLE=y
CONFIG_ALP_SDK_CONSOLE_UNSAFE=y
CONFIG_ALP_SDK_HW_INFO=y
CONFIG_SHELL=y
CONFIG_REBOOT=y
CONFIG_HWINFO=y
```

- [ ] **Step 3: native_sim overlay + conf** — `boards/native_sim_native_64.overlay`:
```dts
/ {
	aliases { alp-uart0 = &uart0; };
};
```
`native_sim.conf` (read-only console on the host build — no real hardware to write):
```
CONFIG_ALP_SDK_CONSOLE=y
CONFIG_REBOOT=n
```
`CMakeLists.txt` wires `EXTRA_CONF_FILE=native_sim.conf` and `generated/alp.conf` per the overlay convention (copy the head of an existing `examples/peripheral-io/*/CMakeLists.txt`).

- [ ] **Step 4: board.yaml** — copy the AEN801 preset from `examples/aen/edgeai-vision-aen/board.yaml`; keep `som.sku` = the AEN801 SKU, `preset: e1m-evk`, core `m55_he` (the bench-verified core). No custom console pins — the console rides the board's existing `chosen { zephyr,shell-uart }` / console UART.

- [ ] **Step 5: Build the example for native_sim (Alif arm compile + smoke)**
```
west build -p -b native_sim/native/64 examples/peripheral-io/alp-console
```
Expected: clean build; `./build/zephyr/zephyr.exe` shows the banner and an `uart:~$` prompt; `alp board` prints identity, `alp mem rd <addr>` works. (Ctrl-C to exit.)

- [ ] **Step 6: Write `docs/console.md`** — cover: enabling (`CONFIG_ALP_SDK_CONSOLE` / `_UNSAFE`), the bench-vs-field profile table, the full command reference with one example each, the companion binding (V2N automatic vs Alif `alp_console_companion_set()`), and the documented follow-ups (`alp companion ota status`, full `alp clk` tree, Linux-side verb port). Link it from the docs index (`git grep -l "peripheral" docs/ | head`).

- [ ] **Step 7: Run the full local gate** (skill: `running-local-ci`)
```
# native_sim twister (full console scope)
west twister -p native_sim -T tests/console --testsuite-root tests/console -O /tmp/tw-console
# format gate
clang-format --dry-run --Werror $(git diff --name-only --diff-filter=ACM main | grep -E '\.(c|h)$')
# ABI / docs / check gates as in running-local-ci
python scripts/check_examples.py   # if present -- verify the new example is well-formed
```
Expected: all green.

- [ ] **Step 8: Commit**
```bash
git add examples/peripheral-io/alp-console docs/console.md
git commit -m "docs(console): alp-console example app + console.md usage guide"
```

---

## Bench Validation (post-merge-candidate, on E1M-AEN801)

Not a code task — the hardware acceptance run (skills: `flashing-and-bench-debugging-v2n` patterns, `docs/bring-up-aen.md`). Record results in the PR:

1. Flash the `alp-console` example (sysbuild + MCUboot per the AEN flow).
2. Open the console UART @115200 8N1.
3. Verify: boot banner → `uart:~$` prompt; `help` lists `alp`; `alp board` shows live EEPROM SKU + hw_rev; `alp gpio read <pin>`; `alp i2c scan <bus>`; `alp adc read <ch>`; `alp companion ver` (CC3501E round-trip); Tab-completion + ↑-history work; an unsafe verb (`alp mem wr`) is present in this bench build.

---

## Self-Review

**Spec coverage:**
- Zephyr-shell foundation → Task 1. ✓
- Two-tier safety (`CONSOLE` / `CONSOLE_UNSAFE`) → Task 1 Kconfig, applied in every group via `IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)`. ✓
- Hardware auto-gating → per-group `CONFIG_GPIO`/`ADC`/companion sub-config + native_sim conf. ✓
- Commands board/gpio/i2c/adc/pwm/mem/clk/companion/reboot → Tasks 1–7. ✓ (`pwm get` and `companion ota status` explicitly descoped with documented reason — no silent cap.)
- Banner reuse (`ALP_SDK_VERSION` + `alp_hw_info_read`) → Task 1 `cmd_board`. ✓
- Companion V2N-singleton vs Alif-register → Task 7. ✓
- native_sim ztest (dummy backend) + bench checklist → Tasks 1–6 tests + Bench section. ✓
- Teaching example + docs → Task 8. ✓

**Placeholder scan:** No "TBD/TODO/handle errors". Three explicit "verify the real symbol name with git grep" notes (CC3501E chip Kconfig, ADC sw-fallback symbol, board SPI alias) are pre-flight checks, not deferred work — each has a concrete fallback.

**Type consistency:** `alp_console_parse_ulong` signature identical across Tasks 1–7. `alp_console_companion_set(cc3501e_t *)` matches between `include/alp/console.h`, `alp_console_companion.c`, and the example. Shell handler signature uniform. `(alp)` root token consistent in every `SHELL_SUBCMD_ADD`.
