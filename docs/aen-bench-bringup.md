@page docs_aen_bench_bringup AEN (E8) bench bring-up + flashing

# E1M-AEN801 (Alif Ensemble E8) — bench bring-up, flashing, and validation results

Everything below was confirmed on **real E1M-AEN801 silicon** during the
2026-06-15 bench session (J-Link PLUS over SWD + the Alif SETOOLS over the
SE-UART). It is the authoritative companion to [`bring-up-aen.md`](bring-up-aen.md)
and [`aen-provisioning.md`](aen-provisioning.md).

## 1. What is validated on silicon

| Subsystem | Result | Notes |
|---|---|---|
| **SE / debug access** | ✅ M55-HE reachable | Generic `Cortex-M55` J-Link device works (CPUID `0x411FD220`, SW-DP IDR `0x4C013477`). **Update (J-Link V9.46, 2026-06-16): the AE822 part-number device profile (`AE822FA0E5597LS0_M55_HE`) also connects fine** — and it is *required* for the Flow D MRAM flash loader (the generic profile has none). An older J-Link DLL may fail to connect with the part-number device; if so, update J-Link or use the generic profile for read/RAM-run (Flows B/C). |
| **Production MRAM flash** | ✅ end-to-end | SETOOLS `app-gen-toc` + `app-write-mram` over the SE-UART; device auto-enters maintenance (no strap); SES loads + boots the ATOC (blink ran at `0x58000000`). |
| **Zephyr boot (alp-sdk image)** | ✅ first light | Boots to the idle thread; "Hello World" read back via RAM console over SWD. |
| **UTIMER counter** (Tier-1.5) | ✅ PASS *after a fix* | As-merged it never counted (read 0); fixed in **PR #158** (missing `alif_utimer_enable_soft_counter_ctrl`). Re-validated: counter advances. |
| **GPIO** (`gpio_dw`, Tier-1) | ✅ PASS (controller) | DDR/DR set+readback correct via the Zephyr GPIO API (J-Link ground truth). Driving an actual **pad** needs the GPIO pad-mux (gpio_dw doesn't apply it). |
| **I2C2 + 24C128 EEPROM** (`i2c_dw`, Tier-1) | ✅ PASS | EEPROM at 0x50 reads back (64 B, `0xff` = blank/unprogrammed) once the pinctrl carries the **pad config** Alif's reference uses — `input-enable` (REN) + `bias-pull-down` (DSC=2). See §3. |
| **PWM** (Tier-1.5) | ✅ PASS | pwm_set_cycles reg readback matches (CNTR_PTR/COMPARE/CTRL), shares the hal_alif UTIMER start-path the counter fix validated. |
| **SPI** (`alif,dwc-ssi-spi`, Tier-2) | ✅ PASS *after a fix* | DWC-SSI stayed in slave mode → `spi_transceive` -116 (TX FIFO full, no SCLK). The Alif SoC gates master mode behind `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`), which upstream never sets. **PR #162** sets it in the driver. Re-validated: `rc=0`, internal-loopback `rx==tx`, CTRLR0=`0x80002007`. See §3. |
| **Ethernet** (`alif,ethernet` / `eth_dwmac`, Tier-1.5) | ✅ PASS *after a fix* | GMAC reachable (HW v5.40) but the DMA reset stalled — no RMII 50 MHz ref-clock. **PR #162** programs the `ETH_CTRL` (`0x4903F080` bit4) source mux with an auto-detect (external pin → fall back to internal PLL). Re-validated: MAC reset completes, iface `admin_up=1`, MAC programmed. See §3. |

## 2. The three flashing / observation flows

| Flow | Use it for | Touches MRAM? | Tooling |
|---|---|---|---|
| **A. Production MRAM flash** | shipping image, QA, re-keying | **Yes** | SETOOLS over the SE-UART (`west flash` = `alif_flash` runner) |
| **B. Console observation** | watching app output during bring-up | No | RAM console over SWD, or SEGGER RTT |
| **C. J-Link RAM-run** | dev/debug iteration without burning MRAM | No | J-Link `loadbin` to ITCM + `go` |
| **D. J-Link MRAM flash** | fast bench iteration onto MRAM with no SE-UART | **Yes** | J-Link MRAM flash loader (**AE822 part-number device profile**) — `loadbin` app + ATOC, then a reset-pin reboot |

A and D both decide *what runs* (D writes the same MRAM image without the SE-UART);
B decides *how you watch it*; C is the fast RAM-only inner loop. On this bench the
only USB serial is the FT232R **SE-UART** (flow A), so the app console is not on USB
— which is why flow B exists. Flow D is the fast MRAM path when the SE-UART is
unavailable or unreliable (e.g. over usbip, or because a board reset re-enumerates
the FT232R — see Flow D).

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

### Flow D — J-Link MRAM flash (direct write, no SE-UART)

**Verified 2026-06-16, J-Link V9.46.** J-Link can program MRAM directly over SWD —
but **only with the AE822 part-number device profile** (`AE822FA0E5597LS0_M55_HE`),
which carries SEGGER's MRAM flash algorithm; the generic `Cortex-M55` device has no
flash loader (that is why earlier notes said "J-Link doesn't write MRAM" — true for
the generic profile, not the part-number one). J-Link sees MRAM as flash **Bank 0 @
`0x80000000`** and needs **no erase** (`Program & Verify`, ~177 KB/s).

This writes the *same bytes* SETOOLS `app-write-mram` burns, so the SE's secure-boot
verification on the next boot is unchanged — **no re-signing, no key step**. Use it
for fast bench iteration, or whenever the SE-UART path is unavailable/unreliable
(over usbip; or because a board reset re-enumerates the FT232R when the bridge
shares the board reset domain — so you can't hold the SE-UART open across a reset,
which is exactly what `app-write-mram`'s maintenance handshake needs).

```bash
# 1. Build the ATOC exactly as Flow A — this produces TWO blobs:
#      build/images/<app>.bin      (the app, linked at its mramAddress)
#      build/AppTocPackage.bin     (the ATOC table)
./app-gen-toc -f build/config/<cfg>.json

# 2. Learn the two MRAM load addresses. The app address is `mramAddress` from
#    the app JSON (HE app partition = 0x80010000). The ATOC address is whatever
#    app-write-mram reports — start it once and read the line, then Ctrl-C (you
#    do NOT need the SE-UART write to succeed, only its printed plan):
./app-write-mram -c /dev/ttyUSB0 -p .
#    -> [INFO] Burning: <app>.bin 0x80010000 AppTocPackage.bin 0x8057f5b0
#       (the ATOC sits near the top of the partition; it can shift per build/config)
```

Then program + reboot with J-Link (the part-number device profile):

```
JLinkExe -device AE822FA0E5597LS0_M55_HE -if SWD -speed 4000 -nogui 1
J-Link> connect
J-Link> loadbin build/images/<app>.bin 0x80010000
J-Link> loadbin build/AppTocPackage.bin 0x8057f5b0
J-Link> verifybin build/images/<app>.bin 0x80010000      # expect "Verify successful."
J-Link> verifybin build/AppTocPackage.bin 0x8057f5b0     # expect "Verify successful."
J-Link> RSetType 2                                        # 2 = reset via the nRESET pin
J-Link> r                                                 # reboots the SES -> re-reads + re-verifies MRAM -> boots the app
J-Link> g
J-Link> exit
```

- The **reset-pin** reboot (`RSetType 2; r`) is what makes the SES re-read MRAM,
  re-verify the ATOC signature, and boot the freshly-written app — a plain
  SYSRESETREQ resets only the M55 and won't re-run the SE boot path.
- After the pin reset J-Link often reports `connect under reset` / `attach failed`.
  That is **normal** on this secure-debug part (the SES re-booted and the app is now
  running); `mem32` memory reads still work even without a clean halt, so you can
  read a SWD witness/RAM console immediately.
- Sanity-check the link base before resetting: `mem32 0x80010000 2` should show the
  app's `SP` then its reset vector (e.g. `…80012xxx`); a reset vector at `…80002xxx`
  means the app linked at `0x80000000` (missing `CONFIG_FLASH_LOAD_OFFSET=0x10000`)
  and will fault on boot.
- Windows JLink.exe drives the same script via `-CommandFile <file.jlink>
  -SelectEmuBySN <sn>`; loadbin reads a host-filesystem path (copy WSL build
  artifacts out to a Windows path first).

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
- **SPI needs the SoC master-mode select set (not just CTRLR0).** The Ensemble
  wraps the DWC_ssi macrocell behind a SoC master/slave select in
  `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`: bit n = SSI*n* master-mode, bit 8+n =
  SS value). Until it's set the controller ignores `CTRLR0.SSI_IS_MST` and never
  drives SCLK as a master (TX FIFO fills, `spi_transceive` → -116). Upstream
  Zephyr's alif SoC layer doesn't program it, so the alp-sdk driver does, in init
  (PR #162). Customers need nothing — it's automatic for master instances. The
  SPI node also carries `clock-frequency` (BAUDR divider) since the clock
  controller doesn't report a rate.
- **Ethernet needs the RMII 50 MHz reference clock selected.** The DWMAC DMA
  soft-reset cannot complete without it. `ETH_CTRL` (`0x4903F080` bit 4) selects
  the source: 0 = external REFCLK pin (P11_0 oscillator, production wiring),
  1 = internal 50 MHz PLL. The driver auto-detects (PR #162): it tries external,
  and if the reset can't complete (no oscillator populated) it falls back to the
  internal PLL — so Ethernet comes up on any board population with no config.
  **Bench note:** this reduced-population board has no working external RMII
  oscillator, so auto-detect selects the internal PLL. Override with
  `CONFIG_ETH_DWMAC_ALIF_RMII_REFCLK_{EXTERNAL,INTERNAL_PLL}` to pin a source.

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `app-write-mram`: `Target did not respond` | SE-UART wiring/baud — 1.8 V adapter, crossed TX/RX, common GND, port = the FT232R SE-UART, baud 57600. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config — write an **app-only** ATOC keeping the factory DEVICE config. |
| `west flash` tries to use J-Link | The carrier's `west flash` must use the **`alif_flash`** runner (SETOOLS over the SE-UART), not a J-Link runner. (Note: J-Link *can* write MRAM directly with the AE822 part-number device profile — see Flow D — but `west flash` is wired to the SETOOLS path.) |
| J-Link MRAM `loadbin` does nothing / "no flash loader" | You used the generic `-device Cortex-M55` (no flash loader). Switch to `-device AE822FA0E5597LS0_M55_HE` for the MRAM flash algorithm (Flow D). |
| No app output over USB | Expected — only the SE-UART is on USB. Use the RAM console (flow B) or RTT. |
| RAM console all-zeros | Read the **`ram_console_buf`** symbol (not `ram_console`); re-resolve from `zephyr.map`; ensure `CONFIG_UART_CONSOLE=n`. |
| J-Link `Could not connect to the target device` | On an older J-Link DLL the AE822 part-number device may fail to connect — switch to the generic `-device Cortex-M55` (Flows B/C). On **V9.46+** the part-number device connects fine and is required for Flow D's MRAM flash loader. |
| Link error `region FLASH overflowed` on a RAM-run app | The overlay used `zephyr,flash = <&itcm>` — use the path-reference form `&itcm` (else `FLASH_SIZE=0`). |
| I2C2 probe times out (`-ETIMEDOUT`) | Bus stuck — pads not driving. Add the I2C pinctrl pad config (§3): `input-enable` + `bias-pull-down`; run at 100 kHz. |
| I2C2 clean NACKs but no device ACKs | The pinctrl is missing **`input-enable`** (REN) so the controller can't sense SDA, or it used `bias-pull-up` (DSC=1) instead of `bias-pull-down` (DSC=2). Match Alif's reference (§3) — then the EEPROM ACKs at 0x50. |
| `spi_transceive` returns `-116` (TX FIFO full, no SCLK) | SoC master-mode not set — `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`) per-instance master bit. The alp-sdk driver sets it in init (PR #162); if you forked the driver, replicate it. |
| `spi_transceive` returns `-EINVAL` with no register programming | No `clock-frequency` for the BAUDR divider and the alif clock controller has no `get_rate`. Set `clock-frequency` on the SPI node (§3). |
| Ethernet `unable to reset hardware` (then MPU fault) | No RMII 50 MHz ref-clock. The driver auto-selects the source (PR #162); if forced to `EXTERNAL` on a board without the oscillator populated, switch to `AUTO` or `INTERNAL_PLL`. |
