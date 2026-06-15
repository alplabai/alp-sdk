@page docs_aen_bench_bringup AEN (E8) bench bring-up + flashing

# E1M-AEN801 (Alif Ensemble E8) — bench bring-up, flashing, and validation results

Everything below was confirmed on **real E1M-AEN801 silicon** during the
2026-06-15 bench session (J-Link PLUS over SWD + the Alif SETOOLS over the
SE-UART). It is the authoritative companion to [`bring-up-aen.md`](bring-up-aen.md)
and [`aen-provisioning.md`](aen-provisioning.md).

## 1. What is validated on silicon

| Subsystem | Result | Notes |
|---|---|---|
| **SE / debug access** | ✅ M55-HE reachable | Generic `Cortex-M55` J-Link device; CPUID `0x411FD220`, SW-DP IDR `0x4C013477`. The Alif part-number device profile fails to connect — use the generic one. |
| **Production MRAM flash** | ✅ end-to-end | SETOOLS `app-gen-toc` + `app-write-mram` over the SE-UART; device auto-enters maintenance (no strap); SES loads + boots the ATOC (blink ran at `0x58000000`). |
| **Zephyr boot (alp-sdk image)** | ✅ first light | Boots to the idle thread; "Hello World" read back via RAM console over SWD. |
| **UTIMER counter** (Tier-1.5) | ✅ PASS *after a fix* | As-merged it never counted (read 0); fixed in **PR #158** (missing `alif_utimer_enable_soft_counter_ctrl`). Re-validated: counter advances. |
| **GPIO** (`gpio_dw`, Tier-1) | ✅ PASS (controller) | DDR/DR set+readback correct via the Zephyr GPIO API (J-Link ground truth). Driving an actual **pad** needs the GPIO pad-mux (gpio_dw doesn't apply it). |
| **I2C2 + 24C128 EEPROM** (`i2c_dw`, Tier-1) | ✅ PASS | EEPROM at 0x50 reads back (64 B, `0xff` = blank/unprogrammed) once the pinctrl carries the **pad config** Alif's reference uses — `input-enable` (REN) + `bias-pull-down` (DSC=2). See §3. |
| **PWM** (Tier-1.5) | ⚙ driver builds | Shares the exact hal_alif UTIMER start-path the counter fix validated; a dedicated reg-readback bench app is pending a binding tweak. |

## 2. The three flashing / observation flows

| Flow | Use it for | Touches MRAM? | Tooling |
|---|---|---|---|
| **A. Production MRAM flash** | shipping image, QA, re-keying | **Yes** | SETOOLS over the SE-UART (`west flash` = `alif_flash` runner) |
| **B. Console observation** | watching app output during bring-up | No | RAM console over SWD, or SEGGER RTT |
| **C. J-Link RAM-run** | dev/debug iteration without burning MRAM | No | J-Link `loadbin` to ITCM + `go` |

A decides *what runs*; B decides *how you watch it*; C is the fast inner loop.
On this bench the only USB serial is the FT232R **SE-UART** (flow A), so the app
console is not on USB — which is why flow B exists.

### Flow A — Production MRAM flash (SETOOLS, no strap/jumper)

```bash
cd <setools>/app-release-exec-linux
# 1. Build the ATOC (app-only config keeps the factory DEVICE config).
./app-gen-toc -f build/config/<cfg>.json
# 2. Write to MRAM over the SE-UART. The device AUTO-enters maintenance
#    (SET_MAINTENANCE_FLAG + reset) — no jumper, no strap. Baud 57600, dynamic.
./app-write-mram -c /dev/ttyUSB0 -p .
```

A clean write ends `100% ... Done`; on reset the SES loads + boots the ATOC
(M55-HE `loadAddress 0x58000000`). `west flash` on the carrier wraps this via the
**`alif_flash`** runner — it does **not** use J-Link. Pre-provisioned Alp Lab
modules ship a dev-signed MCUboot + self-test in slot0 (LCS=DM), so `west flash`
works day-1; the manual path above is only for re-keying or recovering a bare
module.

### Flow B — Seeing the console

| Carrier | HE console UART | Pins |
|---|---|---|
| Alif Ensemble DevKit | UART2 | P1_0 / P1_1 |
| E1M carrier (E1M-EVK) | UART5 | P3_4 / P3_5 |

On a bench whose only USB serial is the SE-UART, use a **RAM console** read over
SWD (`prj.conf`: `CONFIG_RAM_CONSOLE=y`, `CONFIG_RAM_CONSOLE_BUFFER_SIZE=2048`,
`CONFIG_UART_CONSOLE=n`, `CONFIG_CONSOLE=y`). `printk()` accumulates in
`ram_console_buf`; resolve its address (`nm zephyr.elf | grep ram_console_buf`)
and `mem8 <addr>, 0x800` in J-Link, then ASCII-decode. Have each test print one
`RESULT PASS: ...` / `RESULT FAIL: ...` line. SEGGER **RTT** is the live-terminal
alternative over the same SWD link.

### Flow C — J-Link RAM-run (no MRAM write)

The SoC `select`s XIP, so retarget the ROM region to ITCM in the app overlay —
**use the path-reference form** (`<&itcm>` makes `FLASH_SIZE=0` → link overflow):

```dts
/ {
    chosen {
        zephyr,flash = &itcm;          /* NOT <&itcm> */
        /delete-property/ zephyr,code-partition;
    };
};
```

`prj.conf` must set `CONFIG_ALP_SDK=y` (the Tier-1.5 UTIMER Kconfig is gated under
it) + the flow-B RAM console. Build with both module paths
(`-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>`), then:

```
JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1   # GENERIC device
J-Link> halt
J-Link> loadbin build/zephyr/zephyr.bin 0x0   # loadbin's implicit reset re-reads
J-Link> halt                                  # our freshly-loaded ITCM vectors:
J-Link> go                                    # core is already at our reset handler
```

> **Reset caveat:** a J-Link reset asserts **SYSRESETREQ**, which reboots the
> **SES** (not just the M55). Prefer `loadbin`/`go`; don't `reset` mid-loop.

## 3. Board HW requirements found on the bench

- **I2C2 pads need the right pinctrl config (NOT external pull-ups).** The
  on-module 24C128 EEPROM is on **SoC I2C2** (P5_6 `SCL_C` / P5_7 `SDA_C`,
  bridge/DNP-selected) and works on the internal pulls. The original example
  pinctrl set only `pinmux`, so the controller couldn't sense SDA → it NACKed
  every address (looked like a dead/no-device bus). The fix matches Alif's own
  reference i2c pinctrl (`sdk-alif .../ensemble-pinctrl.dtsi`): add
  **`input-enable`** (sets the pad REN bit so `i2c_dw` can read SDA/SCL — ACK
  detect + clock-stretch) and **`bias-pull-down`** (upstream `pinctrl_soc.h`
  encodes this as the pad driver-state-control field **DSC=2**, exactly Alif's
  I2C value — `bias-pull-up` gives DSC=1 and a dead bus; the upstream binding's
  pull naming is effectively inverted vs the Alif pad HW). With that,
  `examples/aen/aen-eeprom-manifest` reads the EEPROM at 0x50 (blank = `0xff`
  until programmed). External pull-ups are only needed for fast-mode (400 kHz);
  100 kHz works on the internal pulls.
- **UTIMER tick rate ≈ 400 MHz, not the 100 MHz placeholder.** The counter
  advanced ~800 k ticks per 2 ms busy-wait → real input ≈ 400 MHz, 4× the
  `clock-frequency = <100000000>` placeholder on the `utimer*` SoC nodes. The
  count is correct; only the µs↔ticks scale is off. Set the verified value from
  the Alif TRM (do not invent it).
- **Housekeeping trio (RTC/TMP/OPTIGA)** is on the slave-only **LPI2C0** this rev
  → Tier-3 (SE-mediated). A next-rev respin moves it to a master-capable I2C
  (LPI2C0 → I2C0 on P7_0/P7_1).

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `app-write-mram`: `Target did not respond` | SE-UART wiring/baud — 1.8 V adapter, crossed TX/RX, common GND, port = the FT232R SE-UART, baud 57600. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config — write an **app-only** ATOC keeping the factory DEVICE config. |
| `west flash` tries to use J-Link | The carrier must use the **`alif_flash`** runner (SETOOLS); J-Link does **not** write MRAM on this part. |
| No app output over USB | Expected — only the SE-UART is on USB. Use the RAM console (flow B) or RTT. |
| RAM console all-zeros | Read the **`ram_console_buf`** symbol (not `ram_console`); re-resolve from `zephyr.map`; ensure `CONFIG_UART_CONSOLE=n`. |
| J-Link `Could not connect to the target device` | You used the Alif part-number device — switch to the generic `-device Cortex-M55`. |
| Link error `region FLASH overflowed` on a RAM-run app | The overlay used `zephyr,flash = <&itcm>` — use the path-reference form `&itcm` (else `FLASH_SIZE=0`). |
| I2C2 probe times out (`-ETIMEDOUT`) | Bus stuck — pads not driving. Add the I2C pinctrl pad config (§3): `input-enable` + `bias-pull-down`; run at 100 kHz. |
| I2C2 clean NACKs but no device ACKs | The pinctrl is missing **`input-enable`** (REN) so the controller can't sense SDA, or it used `bias-pull-up` (DSC=1) instead of `bias-pull-down` (DSC=2). Match Alif's reference (§3) — then the EEPROM ACKs at 0x50. |
