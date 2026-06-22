<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Alp Lab AB -->

# Alp SoM Console

The Alp SoM console gives you an interactive `alp` command tree on the
Zephyr shell, wired at link time from `CONFIG_ALP_SDK_CONSOLE=y`.
No commands are registered in `main()` — the SDK registers everything.

---

## Enabling the console

Add to `prj.conf`:

```kconfig
CONFIG_ALP_SDK_CONSOLE=y   # the whole alp command tree
CONFIG_SHELL=y             # Zephyr shell backend (serial by default)
CONFIG_ALP_SDK_HW_INFO=y   # alp board reads SoM EEPROM at boot
CONFIG_HWINFO=y            # underpins ALP_SDK_HW_INFO
CONFIG_REBOOT=y            # alp reboot
```

The console rides the board's existing `chosen { zephyr,shell-uart }` /
console UART — no custom pins needed.

---

## Safety tiers

| Config symbol                  | Tier         | Commands unlocked                                               |
|--------------------------------|--------------|-----------------------------------------------------------------|
| `CONFIG_ALP_SDK_CONSOLE=y`     | **Field**    | board, mem rd, gpio read, i2c scan/read, adc read, clk, companion ver/ping |
| `+CONFIG_ALP_SDK_CONSOLE_UNSAFE=y` | **Bench** | + mem wr, gpio write, i2c write, pwm set, reboot, companion gpio write |

Enable `_UNSAFE` only in bench / development builds.  Do not ship it in
production firmware — there is no authentication on the serial port.

---

## Boot banner

When `CONFIG_ALP_SDK_CONSOLE=y` is set, `alp_banner.c` (linked
automatically) prints the board identity and Alp SDK version before the
shell prompt appears:

```
*** Alp SDK v0.7.0 — E1M-AEN801 (Alif Ensemble E8)  ***
uart:~$
```

Type `alp` then Tab for sub-command completion, or `alp --help`.

---

## Command reference

All commands live under the `alp` root token on the Zephyr shell.

### `alp board`

Print the SoM identity from the on-module EEPROM (requires
`CONFIG_ALP_SDK_HW_INFO=y`).

```
uart:~$ alp board
Alp SDK 0.7.0  |  E1M-AEN801 v1.0  |  (c) Alp Lab AB
  family : AEN
  serial : AEN801-0042
  mfg    : 2026-01-15
  uptime : 4321 ms
```

### `alp mem rd <addr> [count]`

Read raw memory (32-bit words, one line per word).

```
uart:~$ alp mem rd 0x80010000 4
[80010000] = deadbeef
[80010004] = 20000001
[80010008] = 80080105
[8001000c] = 80080109
```

### `alp mem wr <addr> <value>` *(UNSAFE)*

Write one 32-bit word.

```
uart:~$ alp mem wr 0x40040000 0x1
[40040000] <- 00000001
```

### `alp gpio read <pin>`

Sample a named E1M GPIO pin by its positional index in `alp,pin-array`.

```
uart:~$ alp gpio read 0
pin 0 = 1
```

### `alp gpio write <pin> <0|1>` *(UNSAFE)*

Drive a GPIO pin.

```
uart:~$ alp gpio write 0 1
pin 0 <- 1
```

### `alp i2c scan <bus_id>`

Probe every 7-bit address on an I2C bus (1-byte read).

```
uart:~$ alp i2c scan 0
  0x1e
  0x4a
scan complete, 2 responder(s)
```

### `alp i2c read <bus_id> <addr> <reg> [len] [regbytes]`

Read bytes from an I2C register (up to 16; `len` defaults to 1). `regbytes`
selects the register-address width: `1` (default) for 8-bit-register sensors,
`2` for 16-bit-addressed parts such as the 24C128 EEPROM (the 2-byte address is
sent MSB-first). `len` must be given to pass `regbytes`.

```
uart:~$ alp i2c read 0 0x1e 0x00 4
81 02 00 00
uart:~$ alp i2c read 0 0x50 0x0010 8 2     # 16-bit EEPROM offset 0x0010
00 11 22 33 44 55 66 77
```

### `alp i2c write <bus_id> <addr> <reg> <u8>` *(UNSAFE)*

Write one byte to an I2C register.

```
uart:~$ alp i2c write 0 0x1e 0x00 0x80
0x1e[0x00] <- 0x80
```

### `alp adc read <channel>`

Read a raw ADC channel (12-bit; requires `CONFIG_ALP_SDK_PERIPH_ADC` —
the command compiles out on builds without ADC support).

```
uart:~$ alp adc read 0
adc[0] raw = 2048
```

### `alp pwm set <channel> <period_ns> <duty_ns>` *(UNSAFE)*

Set PWM period and duty cycle.

```
uart:~$ alp pwm set 0 20000000 1000000
pwm[0] period=20000000ns duty=1000000ns
```

### `alp clk`

Report system clock and tick rate.

```
uart:~$ alp clk
sys clock : 400000000 Hz
tick rate : 1000 Hz
```

Note: a full per-IP clock tree (enable/rate-set verbs) is a documented follow-up.

### `alp companion ver`

Query the companion chip firmware version.  On V2N this is the GD32
supervisor; on Alif this is the CC3501E (must be bound first — see
[Companion binding](#companion-binding) below).

On V2N:

```
uart:~$ alp companion ver
GD32 supervisor fw v0.2.6
```

On Alif / CC3501E:

```
uart:~$ alp companion ver
CC3501E protocol v1
```

### `alp companion ping`

Liveness round-trip to the companion.  On V2N a bare GD32 PING frame is
sent and the result is printed:

```
uart:~$ alp companion ping
ping OK
```

On Alif / CC3501E there is no bare PING command; `ping` performs a
`GET_VERSION` round-trip (the same operation as `alp companion ver`) and
prints the CC3501E version string:

```
uart:~$ alp companion ping
CC3501E protocol v1
```

### `alp companion gpio read <0..31>` (V2N only)

Read a GD32 GPIO pin over the supervisor bridge.

```
uart:~$ alp companion gpio read 3
companion pin 3 = 0
```

### `alp companion gpio write <0..31> <0|1>` *(V2N only, UNSAFE)*

Drive a GD32 GPIO pin.

```
uart:~$ alp companion gpio write 3 1
companion pin 3 <- 1
```

### `alp reboot` *(UNSAFE)*

Soft-reset the SoC (calls `sys_reboot(SYS_REBOOT_WARM)`).

```
uart:~$ alp reboot
rebooting...
```

---

## Companion binding

### V2N (RZ/V2N + GD32 supervisor)

The GD32 supervisor is a **singleton** managed inside the SDK.  When
`CONFIG_ALP_SDK_V2N_SUPERVISOR=y` is set (auto-enabled for V2N SoMs),
`alp companion ver` / `alp companion ping` / `alp companion gpio *` work
without any application code.

### Alif (AEN801 + CC3501E)

There is no companion singleton on Alif.  The application opens the
CC3501E and registers the handle with the console **once**:

```c
#include <alp/console.h>
#include <alp/chips/cc3501e.h>
#include "cc3501e_bridge.h"   /* cc3501e_bridge_bringup() */

static cc3501e_t companion;   /* file-static: console borrows it for its life */

static void bind_companion(void)
{
    /* cc3501e_bridge_bringup() opens the inter-chip SPI (bus_id 1,
     * 1 MHz, no CS), the WIFI_EN + nRESET LP-GPIO pads, runs the
     * TI SWRU626 power + reset sequence, and leaves WIFI_EN HIGH. */
    if (cc3501e_bridge_bringup(&companion) == ALP_OK) {
        alp_console_companion_set(&companion);
    }
}
```

`cc3501e_bridge_bringup()` is defined in
`examples/aen/aen-cc3501e-bringup/src/cc3501e_bridge.{c,h}` — copy it
into your application or reference the alp-console example which bundles
it at `examples/peripheral-io/alp-console/src/cc3501e_bridge.{c,h}`.

`alp_console_companion_set()` is declared in `<alp/console.h>`.  Call it
at most once; pass `NULL` to unbind.  The `companion` object **must**
outlive the console (file-static or global storage only — never a local
variable in a function that returns).

---

## Example application

`examples/peripheral-io/alp-console/` is the canonical teaching example.
It enables the console and (on Alif) binds the CC3501E companion.
`main()` is 3 lines; everything else is comments explaining why.

**Build for native_sim (host toolchain, no silicon):**

```sh
west build -p -b native_sim/native/64 examples/peripheral-io/alp-console
./build/zephyr/zephyr.exe
```

**Build for the E1M-AEN801 bench board:**

```sh
west build -p -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/peripheral-io/alp-console
```

After flashing (see [bring-up-aen.md](bring-up-aen.md)):
open the console UART at 115200 8N1, wait for the boot banner, then
type `help` to see `alp` in the command list.

---

## Bench vs field profile

| Feature               | Field build            | Bench build                      |
|-----------------------|------------------------|----------------------------------|
| `CONSOLE`             | y                      | y                                |
| `CONSOLE_UNSAFE`      | **n**                  | **y**                            |
| `mem wr` / `gpio write` | absent               | present                          |
| Boot banner           | printed                | printed                          |
| Companion (AEN)       | bound if cc3501e chips | bound if cc3501e chips           |
| Companion (V2N)       | auto (singleton)       | auto (singleton)                 |

---

## Per-command footprint control

Every command group has its own Kconfig toggle so a tight build can drop
individual verbs without touching the rest.  All symbols default `y` when
`CONFIG_ALP_SDK_CONSOLE=y`, so existing `prj.conf` files are unaffected.

| Symbol                              | Default         | Controls              |
|-------------------------------------|-----------------|-----------------------|
| `CONFIG_ALP_SDK_CONSOLE_CMD_BOARD`  | y               | `alp board`           |
| `CONFIG_ALP_SDK_CONSOLE_CMD_MEM`    | y               | `alp mem`             |
| `CONFIG_ALP_SDK_CONSOLE_CMD_GPIO`   | y               | `alp gpio`            |
| `CONFIG_ALP_SDK_CONSOLE_CMD_I2C`    | y               | `alp i2c`             |
| `CONFIG_ALP_SDK_CONSOLE_CMD_ADC`    | y (needs `ALP_SDK_PERIPH_ADC`) | `alp adc` |
| `CONFIG_ALP_SDK_CONSOLE_CMD_PWM`    | y               | `alp pwm`             |
| `CONFIG_ALP_SDK_CONSOLE_CMD_CLK`    | y               | `alp clk`             |
| `CONFIG_ALP_SDK_CONSOLE_CMD_REBOOT` | y               | `alp reboot`          |
| `CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION` | y if V2N supervisor or CC3501E; else n | `alp companion` |

`CMD_ADC` additionally requires `CONFIG_ALP_SDK_PERIPH_ADC`; it is
automatically absent when ADC support is disabled.

`CMD_REBOOT` and `CMD_PWM` both still require `CONFIG_ALP_SDK_CONSOLE_UNSAFE`
to emit their verbs; the group toggle just controls whether the handler code
is compiled at all.

The dominant binary cost is `CONFIG_SHELL` itself, not individual command
groups.  Dropping groups is most useful when code-size constraints are tight
(e.g. MRAM-constrained Alif M55-HE builds).

---

## Documented follow-ups (out of scope for this release)

- **`alp pwm get`** — read back current PWM period/duty; blocked on
  portable `<alp/pwm.h>` having no duty-read surface (no `get` exists in v1).
- **`alp companion ota status`** — query the CC3501E OTA slot state
  (blocked on the PSA FWU session being owned by the OTA library;
  re-enabling it from the shell needs a mutex, not yet wired).
- **Full `alp clk` sub-tree** — per-node enable / rate-set verbs
  (SoC clock tree is read-only today; write verbs need the clock-control
  driver to expose a shell-safe rate-set path).
- **Linux-side verb port** — `alp board` / `alp mem` as a Linux-side CLI
  tool using `/dev/mem` and the SoM EEPROM sysfs node (deferred to the
  Yocto layer; no ETA).
- **`alp adc read` auto-gate** — the ADC command compiles out when
  `CONFIG_ALP_SDK_PERIPH_ADC` is not set; builds without ADC support are
  clean (this is working as designed, documented here for clarity).
