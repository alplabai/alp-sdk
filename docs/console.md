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

| Config symbol                  | Tier         | Commands unlocked                          |
|--------------------------------|--------------|--------------------------------------------|
| `CONFIG_ALP_SDK_CONSOLE=y`     | **Field**    | board, mem rd, gpio read, i2c scan/read, adc read, pwm get, clk, companion ver/ping |
| `+CONFIG_ALP_SDK_CONSOLE_UNSAFE=y` | **Bench** | + mem wr/fill, gpio write, companion gpio write |

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
SKU:      E1M-AEN801
HW rev:   v1.0
Serial:   AEN801-0042
```

### `alp mem rd <addr> [count]`

Read raw memory (4-byte aligned).

```
uart:~$ alp mem rd 0x80010000 4
0x80010000: de ad be ef  01 00 00 20  05 01 08 80  09 01 08 80
```

### `alp mem wr <addr> <value>` *(UNSAFE)*

Write one 32-bit word.

```
uart:~$ alp mem wr 0x40040000 0x1
```

### `alp mem fill <addr> <len> <byte>` *(UNSAFE)*

Fill a byte range.

```
uart:~$ alp mem fill 0x20000000 64 0xff
```

### `alp gpio read <pin>`

Sample a named E1M GPIO pin by its positional index in `alp,pin-array`.

```
uart:~$ alp gpio read 0
pin 0: 1
```

### `alp gpio write <pin> <0|1>` *(UNSAFE)*

Drive a GPIO pin.

```
uart:~$ alp gpio write 0 1
```

### `alp i2c scan <bus_id>`

Probe every 7-bit address on an I2C bus (1-byte read).

```
uart:~$ alp i2c scan 0
addr 0x1e  acked  (DA9292 PMIC)
addr 0x4a  acked  (board ID ADC)
scan done, 2 responder(s)
```

### `alp i2c read <bus_id> <addr> <reg> <len>`

Read bytes from an I2C register.

```
uart:~$ alp i2c read 0 0x1e 0x00 4
0x00: 81 02 00 00
```

### `alp i2c write <bus_id> <addr> <reg> <byte...>` *(UNSAFE)*

Write bytes to an I2C register.

```
uart:~$ alp i2c write 0 0x1e 0x00 0x80
```

### `alp adc read <channel>`

Read a raw ADC channel (requires `CONFIG_ALP_SDK_PERIPH_ADC` or
`CONFIG_ALP_SDK_ADC_SW_FALLBACK`; the command auto-gates on ADC support).

```
uart:~$ alp adc read 0
ch 0: 2048  (12-bit raw, ~1.65 V @ 3.3 V ref)
```

### `alp pwm get <channel>`

Read the current PWM period and pulse width on a channel.

```
uart:~$ alp pwm get 0
ch 0: period=20000000 ns  pulse=1500000 ns
```

### `alp pwm set <channel> <period_ns> <pulse_ns>` *(UNSAFE)*

Set PWM parameters.

```
uart:~$ alp pwm set 0 20000000 1000000
```

### `alp clk`

Report active clock tree nodes (SoC-specific; sub-commands vary by SoM).

```
uart:~$ alp clk
cpu_clk:   400 MHz
ahb_clk:   200 MHz
apb_clk:   100 MHz
```

### `alp companion ver`

Query the companion chip firmware version.  On V2N this is the GD32
supervisor; on Alif this is the CC3501E (must be bound first — see
[Companion binding](#companion-binding) below).

```
uart:~$ alp companion ver
CC3501E protocol v1
```

### `alp companion ping`

Liveness round-trip to the companion.  On V2N a bare GD32 PING is sent;
on Alif a `GET_VERSION` is used (proves the SPI framing end-to-end).

```
uart:~$ alp companion ping
ping OK
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

### `alp reboot`

Soft-reset the SoC (calls `sys_reboot(SYS_REBOOT_WARM)`).

```
uart:~$ alp reboot
*** rebooting ***
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

After flashing (see [aen-bench-bringup.md](aen-bench-bringup.md)):
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

## Documented follow-ups (out of scope for this release)

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
  neither `CONFIG_ALP_SDK_PERIPH_ADC` nor `CONFIG_ALP_SDK_ADC_SW_FALLBACK`
  is set; builds without ADC support are clean (this is working as
  designed, documented here for clarity).
