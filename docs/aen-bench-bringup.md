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
| **PWM** (Tier-1.5) | ✅ PASS | pwm_set_cycles reg readback matches (CNTR_PTR/COMPARE/CTRL), shares the hal_alif UTIMER start-path the counter fix validated. |
| **SPI** (`alif,dwc-ssi-spi`, Tier-2) | ✅ PASS *after a fix* | DWC-SSI stayed in slave mode → `spi_transceive` -116 (TX FIFO full, no SCLK). The Alif SoC gates master mode behind `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`), which upstream never sets. **PR #162** sets it in the driver. Re-validated: `rc=0`, internal-loopback `rx==tx`, CTRLR0=`0x80002007`. See §3. |
| **Ethernet** (`alif,ethernet` / `eth_dwmac`, Tier-1.5) | ✅ PASS *after a fix* | GMAC reachable (HW v5.40) but the DMA reset stalled — no RMII 50 MHz ref-clock. **PR #162** programs the `ETH_CTRL` (`0x4903F080` bit4) source mux with an auto-detect (external pin → fall back to internal PLL). Re-validated: MAC reset completes, iface `admin_up=1`, MAC programmed. See §3. |

## 2. The three flashing / observation flows

| Flow | Use it for | Touches MRAM? | Tooling |
|---|---|---|---|
| **A. Production MRAM flash (SETOOLS/ISP)** | shipping image, QA, re-keying | **Yes** | SETOOLS over the SE-UART (`west flash` = `alif_flash` runner) |
| **B. Console observation** | watching app output during bring-up | No | RAM console over SWD, or SEGGER RTT |
| **C. J-Link RAM-run** | dev/debug iteration without burning MRAM | No | J-Link `loadbin` to ITCM + `go` |
| **D. J-Link MRAM flash** | fast MRAM burn from the SWD probe (no SE-UART) | **Yes** | J-Link + the **Alif J-Link device support pack** (see below) |

A decides *what runs*; B decides *how you watch it*; C is the fast inner loop;
D is the faster A (burns MRAM straight from the SWD probe).
On this bench the only USB serial is the FT232R **SE-UART** (flow A), so the app
console is not on USB — which is why flow B exists.

> **Flows A and D both burn MRAM** — A drives the burn through the SE over the
> SE-UART (ISP), D drives it through the SWD probe. The earlier blanket claim
> "J-Link does not write MRAM on this part" was bench-specific, not a hardware
> limit: J-Link *can* burn Alif MRAM with the part-number device profile (the MRAM
> loader is built into J-Link V9.46+; no separate pack). **Both flow A and flow D
> are now working on this bench** (flow D enabled 2026-06-17 after a probe swap —
> see § Flow D). Flow D is the day-to-day default now: a burn is ~0.16 s over SWD
> with no SE-UART maintenance-window race / power-cycle dance.

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

### Flow D — J-Link MRAM flash (built-in Alif loader, no SE-UART)

Burns MRAM directly from the SWD probe — **no SE-UART round-trip** — using the MRAM
flash loader that is **built into J-Link V9.46+** itself (no extra pack to install).
It is the fast equivalent of flow A and writes the **exact same two blobs to the exact
same addresses** SETOOLS burns, so the SE re-verifies + boots the image identically (no
re-signing, no keys).

**The one thing that unlocks it: the `AE822FA0E5597LS0_M55_HE` device profile.** The
loader only activates for the part-number device; with the generic `-device Cortex-M55`
(what read/RAM-run use) there is no loader and `loadbin` to MRAM does nothing useful.
That is the whole reason older notes said "J-Link doesn't write MRAM" — true for the
generic profile, false for the part-number one.

**The two blobs** (read the addresses from `app-write-mram`'s own `Burning:` line — they
shift per build/config, don't hardcode):

| Blob | Address | What it is |
|---|---|---|
| `zephyr_<app>_he.bin` | `0x80010000` | the app, linked at its slot0 `mramAddress` (`CONFIG_FLASH_LOAD_OFFSET=0x10000`) |
| `AppTocPackage.bin` | e.g. `0x8057f5b0` | the signed ATOC the SE reads at boot (near the top of the partition) |

> **Flow D needs the MRAM-XIP build, not the ITCM (flow C) build.** The app must link at
> `0x80010000` — sanity-check `xxd -l 8 zephyr.bin`: word 1 (reset vector) must read
> `…80012xxx`. `…80002xxx` means it linked at `0x80000000` and will fault on boot.

```bash
# 1. build (MRAM/slot0 link — board default chosen, NOT the flow-C &itcm overlay) + ATOC
ninja -C <build>                                   # -> zephyr/zephyr.bin
cp <build>/zephyr/zephyr.bin <setools>/build/images/zephyr_<app>_he.bin
./app-gen-toc -f build/config/app-zephyr-he.json   # -> build/AppTocPackage.bin
./app-write-mram -c /dev/ttyUSB0 -p .              # run once, read the "Burning:" addrs, Ctrl-C
```

```
# 2. program from the Alif J-Link (JLinkExe on Linux / JLink.exe on Windows; same script)
si SWD
speed 4000
device AE822FA0E5597LS0_M55_HE          # NOT generic Cortex-M55 — this unlocks the loader
connect
loadbin <app>.bin   0x80010000
loadbin <apptoc>.bin 0x8057f5b0         # use the address from the "Burning:" line
verifybin <app>.bin  0x80010000
verifybin <apptoc>.bin 0x8057f5b0
mem32 0x80010000 2                       # reset vector must read 80012xxx
RSetType 2                               # nRESET PIN — mandatory (see below)
r
g
exit
```
Invoke: `JLinkExe -CommanderScript <script>` (Linux) / `JLink.exe -CommandFile <script>`
(Windows). On success J-Link prints `Program & Verify` + `Verify successful.` for both blobs and
`mem32 0x80010000 = … 80012…`. The post-`r` "connect under reset / Attach to CPU failed"
is **normal** — the pin reset re-booted the SE and the app is now running, so J-Link
can't re-halt the secure core. Read back a witness with a second `mem32` connect (memory
reads work while the CPU runs; register reads error out harmlessly).

**Why `RSetType 2` (nRESET pin) is mandatory.** It re-runs the **SE boot ROM**
(full-chip), which is what loads/verifies/boots the app from MRAM. A plain J-Link reset
(`AIRCR.SYSRESETREQ`) only resets the M55, not the SE, so the new image wouldn't boot.
The pin reset also re-enumerates the FT232R SE-UART (shared reset domain) — which is why
you can't hold `app-write-mram` open across a reset, and why flow D sidesteps the SE-UART
ISP-window race entirely.

**Notes / limits:** J-Link can write **MRAM** but **not peripheral/secure registers**
(those writes are firewalled). Writing only the app (without the matching ATOC) fails
secure-boot verification — always write both consistent blobs.

> **Verified state on *this* Linux bench (alplab-gw, 2026-06-17): flow D WORKS.** The
> original probe (J-Link PLUS S/N 600107451, old firmware) could not connect with
> `-device AE822FA0E5597LS0_M55_HE` — it only worked with the generic `-device
> Cortex-M55` (reads/RAM-run). Swapping to a probe on **J-Link V13 firmware (May 2026,
> matched to DLL V9.50; S/N 603000869)** fixed it: the AE822 profile now connects
> (`Connecting to J-Link ...O.K.` → `Found Cortex-M55 r1p0`), the built-in MRAM loader
> programs + verifies the ATOC over SWD (`Verify successful.`, ~0.16 s @ ~200 KB/s), and
> `RSetType 2` (nRESET pin) re-runs the SE boot ROM so the app boots from MRAM. Bench
> proof: flashed `aen-ethernet-link` over flow D → `RESULT PASS` + DHCP lease
> `192.168.10.137` (server-side dnsmasq lease + ARP `REACHABLE` confirm).
>
> **Two gotchas that block the probe (both bit us):** (1) a version-mismatched probe
> triggers a mandatory **J-Link firmware update on first connect**, and that update
> **times out over a USB hub** (`Communication timed out - Can not execute firmware
> update`) leaving the probe stuck (`Cannot connect to the probe/programmer`, enumerates
> but no session) — plug the probe into a **direct root USB port**, not a hub, so the FW
> update completes. (2) The post-burn `r`/`g` prints "core did not halt / reconnecting" —
> that is **normal** (the pin reset reboots the SE, the app is running, J-Link can't
> re-halt the secure core); read a witness back over the generic device.
>
> Helper: `bench-builds/flash-jlink.sh <build-dir> [read-bytes]` runs this whole flow
> (gen-toc → AE822 connect → loadbin/verify the package at its per-build start address
> from `app-package-map.txt` → `RSetType 2`/`r`/`g` → RAM-console read-back). It writes
> the **single self-contained `AppTocPackage.bin`** (our ITCM-load-via-ATOC apps), not
> the slot0-XIP two-blob variant above.

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
| `west flash` tries to use J-Link | The carrier defaults to the **`alif_flash`** runner (SETOOLS/ISP). J-Link *can* burn MRAM (flow D) but only with the Alif J-Link device pack installed; without it, use `alif_flash`. |
| No app output over USB | Expected — only the SE-UART is on USB. Use the RAM console (flow B) or RTT. |
| RAM console all-zeros | Read the **`ram_console_buf`** symbol (not `ram_console`); re-resolve from `zephyr.map`; ensure `CONFIG_UART_CONSOLE=n`. |
| J-Link `Could not connect to the target device` (Alif part device) | For **read/attach/RAM-run** use the generic `-device Cortex-M55` (attaches to the live core). For **MRAM flash** (flow D) the Alif part device is required *and* needs the Alif J-Link device pack installed (§ Flow D) — absent on this bench. |
| Link error `region FLASH overflowed` on a RAM-run app | The overlay used `zephyr,flash = <&itcm>` — use the path-reference form `&itcm` (else `FLASH_SIZE=0`). |
| I2C2 probe times out (`-ETIMEDOUT`) | Bus stuck — pads not driving. Add the I2C pinctrl pad config (§3): `input-enable` + `bias-pull-down`; run at 100 kHz. |
| I2C2 clean NACKs but no device ACKs | The pinctrl is missing **`input-enable`** (REN) so the controller can't sense SDA, or it used `bias-pull-up` (DSC=1) instead of `bias-pull-down` (DSC=2). Match Alif's reference (§3) — then the EEPROM ACKs at 0x50. |
| `spi_transceive` returns `-116` (TX FIFO full, no SCLK) | SoC master-mode not set — `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`) per-instance master bit. The alp-sdk driver sets it in init (PR #162); if you forked the driver, replicate it. |
| `spi_transceive` returns `-EINVAL` with no register programming | No `clock-frequency` for the BAUDR divider and the alif clock controller has no `get_rate`. Set `clock-frequency` on the SPI node (§3). |
| Ethernet `unable to reset hardware` (then MPU fault) | No RMII 50 MHz ref-clock. The driver auto-selects the source (PR #162); if forced to `EXTERNAL` on a board without the oscillator populated, switch to `AUTO` or `INTERNAL_PLL`. |
