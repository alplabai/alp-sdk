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
| **M55-HP core (second M55)** | ✅ first light (2026-06-17) | The HP core is held in reset at power-on (only the HE core's AP shows a CPUID); released by SES booting an **`M55_HP` ATOC** (`cpu_id=M55_HP`, `loadAddress=0x50000000` = HP ITCM global, vs HE's `0x58000000`). Proven alive by an advancing **SRAM0 liveness beacon** (`0x02000000`: magic `0xA11FE000` + CPUID `0x411FD220` + heartbeat that advances across a re-read) — read over the system/HE AP, not the HP AP. Example `examples/aen/aen-hp-core-smoke`; helper `scripts/bench/aen/flash-jlink-hp.sh`. Unblocks the HE↔HP MHUv2 doorbell. |
| **UTIMER counter** (Tier-1.5) | ✅ PASS *after a fix* | As-merged it never counted (read 0); fixed in **PR #158** (missing `alif_utimer_enable_soft_counter_ctrl`). Re-validated: counter advances. |
| **GPIO** (`gpio_dw`, Tier-1) | ✅ PASS (controller) | DDR/DR set+readback correct via the Zephyr GPIO API (J-Link ground truth). Driving an actual **pad** needs the GPIO pad-mux (gpio_dw doesn't apply it). |
| **I2C2 + 24C128 EEPROM** (`i2c_dw`, Tier-1) | ✅ PASS | EEPROM at 0x50 reads back (64 B, `0xff` = blank/unprogrammed) once the pinctrl carries the **pad config** Alif's reference uses — `input-enable` (REN) + `bias-pull-down` (DSC=2). See §3. |
| **PWM** (Tier-1.5) | ✅ PASS | pwm_set_cycles reg readback matches (CNTR_PTR/COMPARE/CTRL), shares the hal_alif UTIMER start-path the counter fix validated. |
| **SPI** (`alif,dwc-ssi-spi`, Tier-2) | ✅ PASS *after a fix* | DWC-SSI stayed in slave mode → `spi_transceive` -116 (TX FIFO full, no SCLK). The Alif SoC gates master mode behind `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`), which upstream never sets. **PR #162** sets it in the driver. Re-validated: `rc=0`, internal-loopback `rx==tx`, CTRLR0=`0x80002007`. See §3. |
| **Ethernet** (`alif,ethernet` / `eth_dwmac`, Tier-1.5) | ✅ PASS *after a fix* | Real cause of the long no-link: the GMAC DMA descriptor rings + net_buf pool sat in the M55 **DTCM** (`zephyr,sram = &dtcm`), which is **not** on the GMAC DMA bus. Fix: `zephyr,sram = &sram0` (global on-chip SRAM `0x02000000`, CPU addr == DMA addr) + `CONFIG_DCACHE=n`. The PHY power (`E_PHY_PWRDWN` = P15_4), reset (`E_PHY_RESET` = P11_6), and RCSR bit7 `REF_CLK_SEL=1` were already correct. Re-validated end-to-end: DHCP lease `192.168.10.137` (server-side dnsmasq lease + ARP `REACHABLE`). See §3. |
| **UART3** (`ns16550`, Tier-1) | ✅ PASS | Internal loopback. |
| **Counter** (`utimer0`, Tier-1.5) | ✅ PASS | UTIMER0 counter advances. |
| **Counter alarm** (`utimer0` COMPARE-A, Tier-1.5) | ✅ PASS *after a fix* (RAM-run, 2026-06-17) | The COMPARE-A one-shot **alarm** fires + re-arms (`fired=2`). Two bring-up bugs fixed in `counter_alif_utimer.c`: (1) the match interrupt compares the `COMPARE_A_BUF1` **shadow** register (`0xD4`), not the `COMPARE_A` reg (`0xD0`) the driver wrote — so the shadow stayed 0 and bit2 only matched at the start `CNTR==0` tick; (2) the alarm's NVIC line is `comp_a_buf1` (the bit2 event), not `comp_capt_a` (bit0/CAPTURE_A) — so even once bit2 latched its line was never enabled. Regression: `examples/aen/aen-counter-alarm-regcheck`. |
| **WDT** (CMSDK, Tier-1) | ✅ PASS | CMSDK watchdog. |
| **ADC** (`adc_alif`, Tier-2) | ✅ PASS | Single-shot read. |
| **DAC** (`dac_alif`, Tier-2) | ✅ PASS | Write path holds (code-side; analog output bench-unverified). |
| **Camera stack** (`cam`/`csi`/`dphy`/`arx3a0`) | ✅ PASS *(bind)* | All four nodes BIND + the v4.4-ported drivers load; `cam` instantiation is DT-blocked and live capture is HW-blocked (no sensor wired). |
| **Ethos-U85** (NPU) | ✅ PASS | ID `0x20007001`. |
| **Ethos-U55-HE** (NPU) | ✅ PASS | ID `0x10104201`. |
| **NPU inference** (TFLM + Ethos-U85) | ✅ PASS | Tiny fixture runs to completion. |
| **PDM mics** | ✅ PASS | Live varying PCM = real audio. |
| **I2S TX** (`i2s3`) | ✅ PASS | Clocks the tone out with the 76.8 MHz audio clock (audible amp output pends the 74LVC157 mux + TAS2563 config). |
| **Quadrature encoder** (`qenc`) | 🟡 PARTIAL *(HW-gated)* | Driver reads clean; count is static until the encoder is physically spun. Not a code/Flow-D bug. |
| **SD card** (DWC SDHC) | 🟡 PARTIAL *(HW-gated)* | SDHC inits but the card is unreachable until the EVK SDIO 74LVC157 mux (EN=IO20 / SEL=IO21, both CC3501E-side) is routed and a card is inserted. Not a code/Flow-D bug. |
| **GPU2D** (`<alp/gpu2d.h>` sw_fallback) | ✅ PASS (RAM-run, 2026-06-17) | Portable 2D surface on the M55-HE via the priority-0 pure-C software fallback (the D/AVE 2D HW backend is opt-in + bench-unverified). `fill_rect` + clip, `blit`, and all four `blend` modes (REPLACE/SRC_OVER/ADDITIVE/MULTIPLY) produce **exact** expected pixels on silicon. Example: `examples/aen/aen-gpu2d-bench`. The D/AVE 2D hardware backend (`alif_dave2d.c`) is the separate bucket-C item. |

All 17 aen-* bench apps were flashed over flow D and booted on real E8: **15 PASS,
2 PARTIAL** (both hardware-gated, not code/flow-D bugs).

## 2. The four flashing / observation flows

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

> **Runnable helpers.** The shell helpers that drive all four flows below
> (build, Flow A `flash-run.sh`, Flow C `ram-run.sh`, Flow D `flash-jlink.sh` +
> the `flash-all-flowd.sh` batch, plus `reread.sh`) are checked into
> [`scripts/bench/aen/`](../scripts/bench/aen/) with host-specific values
> (`SETOOLS_DIR`, `SE_UART`, the J-Link probe) sanitized into a sourced
> `bench-env.sh`. See [`scripts/bench/aen/README.md`](../scripts/bench/aen/README.md)
> for per-script usage and the env-var table. **The Alif SETOOLS are
> license-gated and are not redistributed by alp-sdk** — obtain them from Alif
> and `export SETOOLS_DIR=...` before running Flow A/D.

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
> Helper: `scripts/bench/aen/flash-jlink.sh <build-dir> [read-bytes]` runs this whole
> flow (gen-toc → AE822 connect → loadbin/verify the package at its per-build start
> address from `app-package-map.txt` → `RSetType 2`/`r`/`g` → RAM-console read-back). It
> writes the **single self-contained `AppTocPackage.bin`** (our ITCM-load-via-ATOC apps),
> not the slot0-XIP two-blob variant above. See
> [`scripts/bench/aen/README.md`](../scripts/bench/aen/README.md) for all four flows.
>
> **Two-blob (slot0-XIP) helper — validated 2026-06-17.** For an app linked into MRAM
> slot0 (a real NPU model that overflows ITCM), `scripts/bench/aen/flash-jlink-mramxip.sh`
> runs the two-blob flow (app → `0x80010000` + the signed ATOC → its parsed address). Two
> facts the bench pinned down: the app entry's `mramAddress` is the **full** address
> `0x80010000` (the `0x10000` *offset* gives SETOOLS `Invalid Global Address`), and the
> image needs **`CONFIG_USE_DT_CODE_PARTITION=y`** so it links at the slot0 offset
> (`0x8001xxxx` reset vector) instead of the MRAM base (`0x8000xxxx`, which faults). Proven
> by `examples/aen/aen-npu-inference-person-mram` (the real `person_detect` MobileNet run
> from MRAM → `RESULT PASS`).

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
- **Ethernet DMA buffers must live in global SRAM0, not the M55 DTCM.** The long
  no-link was traced to the GMAC DMA descriptor rings + net_buf pool sitting in
  the M55 **DTCM** (`zephyr,sram = &dtcm`), which is **not** reachable on the GMAC
  DMA bus — so the MAC never saw valid descriptors. Fix: `zephyr,sram = &sram0`
  (global on-chip SRAM `0x02000000`, where the CPU address equals the DMA address)
  + `CONFIG_DCACHE=n`. The PHY power (`E_PHY_PWRDWN` = P15_4 lpgpio), PHY reset
  (`E_PHY_RESET` = P11_6 gpio11), and the RCSR bit7 `REF_CLK_SEL=1` ref-clock
  select were all already correct — the earlier "PHY RX path / `ANLPAR=0` / scope
  the REF_CLK" diagnosis was a red herring (a bad cable plus the DTCM starvation).
  Re-validated end-to-end: DHCP lease `192.168.10.137` (server-side dnsmasq lease
  + ARP `REACHABLE`).
- **Generalizable: any DMA-master block needs its buffers in global SRAM.** On the
  E8 M55, any DMA-master block (GMAC, the Ethos-U NPU, the SDHC) needs its
  DMA-visible buffers in global **SRAM0/SRAM1**, never the default DTCM.
- **Audio clocks (I2S bit clock + HP-PDM functional clock) are now in the Tier-1.5
  clockctrl west-patch, not in example pokes.** The upstream Alif clock controller
  (`drivers/clock_control/clock_control_alif.c`) only flips per-peripheral *gate*
  bits — it never enables the audio *master* source, so the I2S and PDM examples
  used to poke raw CGU / EXPMST0 addresses by hand. That is now folded into the
  clockctrl, carried as
  `zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch`
  (`zephyr/patches.yml`, applied with `west patch apply`). On `clock_control_on()`
  of an I2S or HP-PDM clock the patched driver now: (a) **GROUNDED, bench-proven** —
  enables the CGU master **76.8 MHz / HFOSCx2** (`cgu_base + 0x14`, bit 24) and, for
  the EXPMST0-domain HP PDM, the `EXPMST0_CTRL` **IPCLK_FORCE/PCLK_FORCE** bits
  (`clkctl_per_slv_base + 0x00`, bits 31|30); and on `clock_control_set_rate()` it
  programs (b) **BENCH-UNVERIFIED** the `I2Sx_CTRL` bit-clock divider
  (`76.8 MHz / sclk` into the 10-bit `CKDIV` field) so `i2s_dw.c` stops getting
  `-ENOSYS`. The (b) divider field layout is taken from the Alif `i2s_sync`
  reference but is **not yet confirmed** against the DFP/TRM for the DesignWare
  `i2s_dw` path — it is a separable hunk in the patch so the bench step can land
  (a) and hold (b) if the achieved sample rate regresses. (The 76.8 MHz oscillator
  itself is SE-managed, so the PDM may still need the `se_services`/MHU clock
  request even with the CGU `CLK_ENA` bit set — see the PDM example's PARTIAL note.)

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `app-write-mram`: `Target did not respond` | SE-UART wiring/baud — 1.8 V adapter, crossed TX/RX, common GND, port = the FT232R SE-UART, baud 57600. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config — write an **app-only** ATOC keeping the factory DEVICE config. |
| `west flash` tries to use J-Link | The carrier defaults to the **`alif_flash`** runner (SETOOLS/ISP, now flow A / fallback). J-Link *does* burn MRAM (flow D) with the `AE822FA0E5597LS0_M55_HE` device profile — the loader is built into J-Link V9.46+, no separate pack. |
| No app output over USB | Expected — only the SE-UART is on USB. Use the RAM console (flow B) or RTT. |
| RAM console all-zeros | Read the **`ram_console_buf`** symbol (not `ram_console`); re-resolve from `zephyr.map`; ensure `CONFIG_UART_CONSOLE=n`. |
| J-Link `Could not connect to the target device` (Alif part device) | For **read/attach/RAM-run** use the generic `-device Cortex-M55` (attaches to the live core). For **MRAM flash** (flow D) the `AE822FA0E5597LS0_M55_HE` part device is required — it unlocks the built-in MRAM loader (J-Link V9.46+ DLL, probe on matched V13 firmware); the AE822 profile won't connect on an old-firmware probe (§ Flow D). |
| Link error `region FLASH overflowed` on a RAM-run app | The overlay used `zephyr,flash = <&itcm>` — use the path-reference form `&itcm` (else `FLASH_SIZE=0`). |
| I2C2 probe times out (`-ETIMEDOUT`) | Bus stuck — pads not driving. Add the I2C pinctrl pad config (§3): `input-enable` + `bias-pull-down`; run at 100 kHz. |
| I2C2 clean NACKs but no device ACKs | The pinctrl is missing **`input-enable`** (REN) so the controller can't sense SDA, or it used `bias-pull-up` (DSC=1) instead of `bias-pull-down` (DSC=2). Match Alif's reference (§3) — then the EEPROM ACKs at 0x50. |
| `spi_transceive` returns `-116` (TX FIFO full, no SCLK) | SoC master-mode not set — `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`) per-instance master bit. The alp-sdk driver sets it in init (PR #162); if you forked the driver, replicate it. |
| `spi_transceive` returns `-EINVAL` with no register programming | No `clock-frequency` for the BAUDR divider and the alif clock controller has no `get_rate`. Set `clock-frequency` on the SPI node (§3). |
| Ethernet links but never gets a lease / no traffic | GMAC DMA descriptor rings + net_buf pool are in the M55 **DTCM** (`zephyr,sram = &dtcm`), off the DMA bus. Move them to global SRAM0: `zephyr,sram = &sram0` + `CONFIG_DCACHE=n` (§3). Applies to any DMA-master block (GMAC/NPU/SDHC). |
| I2S TX never clocks out / PDM `dmic_read` → `-EAGAIN` (FIFO=0) | The CGU master **76.8 MHz** source and (for the HP PDM) the `EXPMST0_CTRL` IPCLK/PCLK force bits are not set. These are now enabled by the Tier-1.5 clockctrl west-patch (`west patch apply`; §3) on `clock_control_on()` — confirm the patch is applied. The 76.8 MHz oscillator itself is SE-managed, so the PDM may also need the `se_services`/MHU clock request even with the CGU bit set. |
| I2S sample rate looks wrong (pitch off) | The `I2Sx_CTRL` `CKDIV` divider the clockctrl `.set_rate` programs is **BENCH-UNVERIFIED** (field layout from the Alif `i2s_sync` reference, not the DFP/TRM). Confirm the divider width/position + N-vs-(N-1) convention against the Alif DFP/TRM; the hunk is separable in the patch so it can be held. |
