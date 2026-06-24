# Alp SoM Console — Design

- **Date:** 2026-06-21
- **Branch:** `feat/alp-som-console` (worktree, based on `feat/aen801-lcd-bringup`)
- **Status:** Approved design, pre-implementation
- **Target now:** Alif Ensemble AEN (Zephyr / Cortex-M55), bench-validated on E1M-AEN801.
  Portable to V2N-M33 (also Zephyr) with no new code.

## 1. Problem / Goal

alp-sdk firmware on the Alif SoMs has no interactive console. The UART is a
passive output channel (boot ROM banner, `printf`, the SYS_INIT identity
banner from `src/zephyr/alp_banner.c`). Examples are one-shot. There is no
way to poke a register, toggle a GPIO, scan an I2C bus, or query the companion
chip from a live unit on the bench.

Goal: a single, professional, Linux-like UART console, shared across SoMs,
that serves four uses the maintainer confirmed — **bench debugging**,
**field/production diagnostics**, **teaching example**, and **runtime
control**.

Non-goal: the V2N A55/Linux path. Those boards already have a full shell from
Linux. (A future follow-up may ship the same diagnostic verbs as a Linux
userspace CLI; out of scope here.)

## 2. Foundation Decision

Build on the **Zephyr shell subsystem** (`CONFIG_SHELL`), not a hand-rolled
REPL. Rationale:

- It is the most Linux-like option for free: line editing, command history,
  tab-completion, `Ctrl`-key metakeys, argument parsing, colours, a command
  tree, and a serial backend.
- It is the least code — we write commands, not an input stack.
- It matches the SDK "consume upstream, don't reinvent" rule
  (`securing-the-alp-sdk-position`, ADR 0017). A custom REPL would only earn
  its keep on a non-Zephyr / bare-metal target, which Alif is not.

The Alp value-add is a command tree rooted at **`alp`** whose commands speak
the **portable `<alp/*>` layer** (instance IDs like `E1M_*` / `BOARD_*`,
`alp_hw_info_read()`, the companion abstraction) rather than raw Zephyr device
names. Zephyr's own `kernel` / `device` shell modules stay available for
thread/stack/uptime introspection; we do not duplicate them.

## 3. Safety Model

Two-tier, compile-time, Kconfig-driven (no runtime token — baking a secret in
firmware violates the SDK security rule, and a flat "everything on" foot-guns a
live customer board):

- **`CONFIG_ALP_SDK_CONSOLE`** (default `n`) — enables the console and all
  **read-only** verbs the board supports.
- **`CONFIG_ALP_SDK_CONSOLE_UNSAFE`** (default `n`, `depends on
  ALP_SDK_CONSOLE`) — adds **write / destructive** verbs (`mem wr`, `gpio
  write`, `pwm set`, companion writes, `reboot`).

Each command **group also auto-gates on hardware presence** — the same way
Zephyr shell modules gate on devicetree. A group compiles only when its
substrate is configured (e.g. the companion group only when a companion is
configured; ADC only with `CONFIG_ADC`). Commands you cannot run never appear
in `help` → clean output, good CX, and zero per-group Kconfig knobs to
maintain.

Build profiles:
- **Bench:** `ALP_SDK_CONSOLE=y`, `ALP_SDK_CONSOLE_UNSAFE=y`.
- **Field:** `ALP_SDK_CONSOLE=y`, `ALP_SDK_CONSOLE_UNSAFE=n` — write verbs are
  physically absent from the binary.

## 4. Command Tree

Grammar: `alp <group> <verb> [args]` (Linux-like, tab-completable).

| Command | Verbs | Tier | Gate |
|---|---|---|---|
| `alp board` | (summary) / `id` / `uptime` / `reset-cause` / `ver` | safe | always |
| `alp gpio` | `read <pin>` / `mode <pin> <in\|out\|...>` / **`write <pin> 0\|1`** | read safe, write unsafe | `CONFIG_GPIO` |
| `alp i2c` | `scan <bus>` / `read <bus> <addr> <reg> [n]` / **`write <bus> <addr> <reg> <val>`** | read safe, write unsafe | i2c instance present |
| `alp adc` | `read <ch>` | safe | `CONFIG_ADC` |
| `alp pwm` | `get <ch>` / **`set <ch> <duty%> [hz]`** | get safe, set unsafe | `CONFIG_PWM` |
| `alp mem` | `rd <addr> [n]` / **`wr <addr> <val> [width]`** | rd safe, wr unsafe | always |
| `alp clk` | (dump) | safe | always (best-effort) |
| `alp companion` | `ver` / `ping` / `gpio read <p>` / **`gpio write <p> <v>`** / `ota status` | mixed | companion configured |
| `alp reboot` | (warm) / `bootloader` | unsafe | always |

Notes:
- `alp board` reuses the **identity source the banner already uses**:
  `CONFIG_ALP_SDK_VERSION` for the SDK version and, when
  `CONFIG_ALP_SDK_HW_INFO=y`, `alp_hw_info_read()` for the live EEPROM SKU /
  hw-rev / serial / family / mfg-date. When HW_INFO is off it falls back to
  `CONFIG_BOARD`. The console does **not** duplicate the banner; the SYS_INIT
  banner still prints at boot and the shell prompt follows it.
- `alp board reset-cause` uses Zephyr `hwinfo_get_reset_cause()` when
  `CONFIG_HWINFO`; absent that, it reports "unknown" (best-effort, never
  fails).
- `alp gpio` / `alp i2c` / `alp adc` / `alp pwm` call the portable opens
  (`alp_gpio_open`, `alp_i2c_open`, `alp_adc_open`, `alp_pwm_open`) with
  `E1M_*` / `BOARD_*` instance IDs from `<alp/e1m_pinout.h>` — that portability
  is the value over Zephyr's raw `i2c`/`gpio` device-name commands.
- `alp mem` is a width-aware (`b`/`h`/`w`) volatile peek/poke. `rd` is the
  always-available debug primitive; `wr` is unsafe-gated.

### Companion binding (the one cross-SoM nuance)

`alp companion` dispatches through a thin internal shim
(`alp_console_companion.c`), not a new public `<alp/companion.h>` (YAGNI for
v1 — but it seeds one):

- **V2N:** binds the GD32 supervisor **singleton** automatically. That
  singleton already exists and auto-inits under `CONFIG_ALP_SDK_V2N_SUPERVISOR`
  (default `y` on `ALP_SOC_RENESAS_RZV2N_N44 && ALP_SDK_CHIP_GD32G553`). Zero
  app code — the shim calls `gd32g553_*` against it.
- **Alif:** CC3501E has **no auto-singleton** (the app constructs its
  `cc3501e_t` over its own `alp_spi_t`). So the console exposes a tiny
  registration hook — `alp_console_companion_set(cc3501e_t *)` — that the app
  calls once after `cc3501e_init()`. The shim then routes `ver` →
  `cc3501e_get_version`, `ping` / `gpio` / `ota status` → `cc3501e_request`.
  Until registered, `alp companion *` prints `companion not registered` rather
  than dereferencing NULL.

This keeps one portable command surface while being honest that the two SoMs
reach their companion differently.

## 5. Components / Files

Mirrors the existing banner layout (`src/zephyr/`, wired in `zephyr/CMakeLists.txt`):

```
src/zephyr/console/
  alp_console.c            # `alp` root subcmd set + `board` + `mem` + `clk` + `reboot`
  alp_console_gpio.c       # `alp gpio`
  alp_console_i2c.c        # `alp i2c`
  alp_console_periph.c     # `alp adc` + `alp pwm`
  alp_console_companion.c  # `alp companion` + the V2N/Alif dispatch shim
zephyr/Kconfig             # + CONFIG_ALP_SDK_CONSOLE, + CONFIG_ALP_SDK_CONSOLE_UNSAFE
zephyr/CMakeLists.txt      # zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE console/*.c)
examples/peripheral-io/alp-console/   # teaching example (~50% comment ratio)
  src/main.c               # opens the companion (Alif: registers it), then idles
  board.yaml               # enables console + companion + hw_info for AEN801
  prj.conf                 # CONFIG_ALP_SDK_CONSOLE(+_UNSAFE) for the bench build
  CMakeLists.txt
  boards/native_sim_native_64.overlay
  generated/alp.conf       # native_sim overlay (per the conf-GLOB convention)
  native_sim.conf
docs/                      # console usage doc (via the updating-docs skill at finish)
tests/                     # native_sim ztest exercising command registration + safe verbs
```

### Registration mechanism

Use Zephyr's **decentralized** subcommand API so each group lives in its own
file: `alp_console.c` creates the root set and registers `alp`
(`SHELL_SUBCMD_SET_CREATE` + `SHELL_CMD_REGISTER(alp, ...)`); each group file
adds itself with `SHELL_SUBCMD_ADD((alp), <group>, ...)`. Within a group, the
static subcmd set lists read verbs unconditionally and wraps write verbs in
`#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)`.

### Kconfig (placed inside the existing `if ALP_SDK` block, near the banner)

```
config ALP_SDK_CONSOLE
    bool "Alp SoM interactive console (Zephyr shell + `alp` commands)"
    depends on ALP_SDK
    select SHELL
    help
      Register the `alp` command tree on the Zephyr shell ...

config ALP_SDK_CONSOLE_UNSAFE
    bool "Allow write / destructive `alp` console commands"
    depends on ALP_SDK_CONSOLE
    default n
    help
      Adds mem wr, gpio/companion write, pwm set, reboot ...
```

The console reaches the operator over the Zephyr serial shell backend
(`CONFIG_SHELL_BACKEND_SERIAL` + the board's `chosen { zephyr,shell-uart }`),
which on AEN is the same console UART the banner prints to.

## 6. Error Handling

- Every command validates argc/arg ranges and returns a non-zero shell error
  with a one-line `shell_error()` message; never asserts on user input.
- Hardware-absent paths (no companion registered, EEPROM unreadable, HWINFO
  absent) degrade gracefully with a clear message, never a fault.
- The banner contract is preserved: nothing the console adds may fail a boot.

## 7. Testing

- **native_sim (twister, the load-bearing local gate):** a `tests/` ztest
  enables `CONFIG_ALP_SDK_CONSOLE` (+`_UNSAFE`), drives the Zephyr **dummy
  shell backend**, runs `shell_execute_cmd("alp board")`,
  `"alp mem rd <addr>"`, etc., and asserts they register and return success on
  the `sw_fallback` backends. Runs in the existing
  `reference_local_twister_invocation` scope.
- **Bench (E1M-AEN801, per `flashing-and-bench-debugging` / `bring-up-aen`):**
  flash, open the console UART @115200, verify: boot banner → `uart:~$`
  prompt; `help` lists `alp`; `alp board` shows identity; `alp gpio read`,
  `alp i2c scan`, `alp adc read`, `alp companion ver` (CC3501E ping);
  tab-completion and ↑-history work.

## 8. Scope Boundary

- **In:** Alif AEN (Zephyr/M55) now; the same module drops onto V2N-M33 later
  with no new code (companion auto-binds the GD32 singleton there).
- **Out:** V2N A55 / Linux (has bash); a Linux-side port of the verbs is a
  separate future follow-up.

## 9. CI / Conventions

- C house style: clang-format-14 tabs + Consecutive alignment
  (`applying-the-alp-sdk-c-house-style`).
- Portable-API rule: example code uses `<alp/*>` only; `cc3501e_*` /
  `gd32g553_*` appear solely inside the console's internal companion shim and
  the dedicated companion example, never in general example app code
  (`feedback_portable_peripheral_api`).
- File headers: `Copyright 2026 Alp Lab AB` + SPDX only (no personal
  attribution, no Claude footer).
- Branding: "Alp", never "ALP".
- Run the local CI gates (`running-local-ci`) before any push.
