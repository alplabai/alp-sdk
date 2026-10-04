# E1M-X V2N family

> Renesas RZ/V2N-based SoMs in the E1M-X (45 × 65 mm) form factor.

This page is the single landing point for firmware engineers
working with the V2N module.  Skim it once, then follow the
deep-link of whatever you're doing.

## SKUs

| SKU            | Memory                                | Status     |
|----------------|---------------------------------------|------------|
| `E1M-V2N101`   | 32 Gbit LPDDR4X + 32 Gbit eMMC        | production |
| `E1M-V2N102`   | 64 Gbit LPDDR4X + 128 Gbit eMMC       | production |
| `E1M-V2N103`   | 32 Gbit LPDDR4X + 128 Gbit eMMC       | production |

All three SKUs share the same silicon + PCB.  Pick by memory budget.

## What's on the module

| Role                    | Part                       | Bus / signal     | Driver                                  |
|-------------------------|----------------------------|------------------|-----------------------------------------|
| Application SoC         | Renesas RZ/V2N (R9A09G056N44) | -- | (vendor HAL)                                  |
| Companion supervisor MCU| GigaDevice GD32G553MEY7TR  | SPI + I2C bridge | [`<alp/chips/gd32g553.h>`](../../include/alp/chips/gd32g553.h) |
| Primary PMIC            | Qorvo ACT88760-120.E1      | I2C `0x25/0x26`  | [`<alp/chips/act8760.h>`](../../include/alp/chips/act8760.h) |
| Secondary PMIC          | Renesas DA9292             | I2C `0x1E`       | [`<alp/chips/da9292.h>`](../../include/alp/chips/da9292.h) |
| Optional buck (LPDDR4X) | TI TPS628640 (1×, optional)| I2C `0x4D`       | [`<alp/chips/tps628640.h>`](../../include/alp/chips/tps628640.h) |
| Clock generator         | Renesas / IDT 5L35023B     | I2C `0x69`       | [`<alp/chips/clk_5l35023b.h>`](../../include/alp/chips/clk_5l35023b.h) |
| RTC                     | Micro Crystal RV-3028-C7   | I2C `0x52`       | Linux `/dev/rtc0` (kernel `rtc-rv3028`) -- see below |
| Temperature sensor      | TI TMP112                  | I2C `0x40`       | [`<alp/chips/tmp112.h>`](../../include/alp/chips/tmp112.h) |
| Secure element          | Infineon OPTIGA Trust M    | I2C `0x30`       | [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h) |
| EEPROM (SoM manifest)   | Onsemi N24S128             | I2C `0x50` (ALP_E1M_I2C0) | [`<alp/chips/eeprom_24c128.h>`](../../include/alp/chips/eeprom_24c128.h) |
| Wi-Fi 6 + BLE 5.4       | Murata LBEE5HY2FY-922      | SDIO + UART + I2S | [`<alp/chips/murata_lbee5hy2fy.h>`](../../include/alp/chips/murata_lbee5hy2fy.h) |
| Ethernet PHY 0          | Realtek RTL8211FDI-VD-CG   | RGMII + MDIO     | [`<alp/chips/rtl8211fdi.h>`](../../include/alp/chips/rtl8211fdi.h) |
| Ethernet PHY 1          | Realtek RTL8211FDI-VD-CG   | RGMII + MDIO     | (same driver, second instance)          |
| eMMC                    | (variant per SKU)          | Renesas SD0      | Zephyr SD subsystem                     |
| NOR flash               | (variant per SKU)          | Renesas xSPI0    | Zephyr flash subsystem                  |

Full chip catalogue + manifest URLs:
[`metadata/chips/`](../../metadata/chips/).
Per-SKU populated parts: [`metadata/e1m_modules/E1M-V2N10{1,2,3}.yaml`](../../metadata/e1m_modules/).

## Real-time clock {#real-time-clock}

The on-module RV-3028-C7 is the RTC of record, bound as `/dev/rtc0`
(kernel `rtc-rv3028`, `CONFIG_RTC_DRV_RV3028=y`) -- use `hwclock`/`date`
from userspace. **CA55 (Linux) is the sole master of the whole
RIIC8/BRD_I2C bus** the RTC and every other BRD_I2C device sit on
(`metadata/e1m_modules/v2n/core-ownership.yaml`); the CM33 must never
issue I2C transactions there. **Confirmed 2026-09-29 on E1M-V2M103
2026W38-0001:** the RV-3028-C7 has no time backup across a power cycle
unless the carrier fits pad `P10` (VBACKUP); no `trickle-resistor-ohms`
is configured for it. Without a backup source, the image resyncs the
RTC on every boot: `systemd-timesyncd` pulls wall-clock over NTP once
networked, and the kernel writes the result back to `rtc0`
(`hwclock -w`-equivalent via `systemd-time-wait-sync` / `hwclock` unit)
so `/dev/rtc0` reads the corrected time on the next cold boot. The
alarm INT line isn't wired to a kernel interrupt yet -- that remains an
open follow-up.

The RZ/V2N's own RTC (RTCA-3, RTXIN/RTXOUT) is a second, SoC-internal
timebase and is enabled in the SoM devicetree (`&rtc` in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`).
RTXIN has no discrete 32.768 kHz crystal -- it is fed by the SE1 output
of the clock generator, and U-Boot must correct that output to
32.768 kHz on every boot before Linux probes `&rtc`; see the
clock-generator fixup section below for the mechanism and why a stale
build would see this RTC time out instead. `&rtc` probes before
`rv3028` and is pinned to `/dev/rtc1` (`rtc1` alias) so `rv3028` keeps
`/dev/rtc0` -- see `e1m-v2n-som.dtsi`'s `aliases` block.

## On-module clock-generator fixup {#on-module-clock-generator-fixup}

The on-module 5L35023B programmable clock generator (RIIC8/BRD_I2C,
`0x69`) ships an OTP image whose single-ended output routing is wrong
for this SoM:

| Output | Feeds                                    | As-shipped (wrong) | Corrected |
|--------|-------------------------------------------|---------------------|-----------|
| SE1    | SoC RTXIN (RTCA-3) + the Wi-Fi module's 32k LPO input | 24.576 MHz | 32.768 kHz |
| SE3    | On-module audio clock                      | 22.5792 MHz         | 24.576 MHz |

Bench-confirmed (2026-09-24): with the as-shipped OTP values, the SoC
RTC fails to start (`error -ETIMEDOUT: Failed to setup the RTC!`). Two
volatile register writes fix it (register `0x24`: SE1 DCO select;
register `0x21`: SE3 source select); after them the SoC RTC counts at
32.768 kHz. Both registers are OTP-shadow registers -- the writes take
effect immediately but **revert on power-cycle** (the OTP itself cannot
be re-burned in-system) -- so U-Boot applies them on **every** boot,
early in `board_late_init()`, before the DEEPX rail sequencing step and
before Linux starts
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0007-rzv2n-dev-ALP-E1M-clkgen-otp-fixup.patch`).
The fixup only writes when both registers read the exact as-shipped
values; any other readback (already fixed, a differently configured
part, or a communication error) is left untouched and only logged.

This is a runtime workaround. **Production builds should instead use a
Renesas factory dash code that carries the corrected OTP image**,
removing the need for the U-Boot fixup entirely.

## Reach the GD32 supervisor

The V2N's GD32G553 supervisor MCU owns half the E1M-edge
peripherals (eight PWM channels, dual ADC + DAC bank, the Wi-Fi/BT
REG_ON pins, OPTIGA reset, 18 IO routes to the E1M edge).  The
host driver speaks both transports:

* **SPI fast path** -- Renesas SCI7 Simple-SPI master on
  `P76/P77/P96/P97` ↔ GD32 slave on `PA8/9/10/PB15`.  Use for
  high-frequency telemetry + PWM updates.
* **I2C management path** -- on BRD_I2C (`P07/P06`), GD32 at
  7-bit `0x70`.  Use when you're already on BRD_I2C for the
  PMIC fleet.

**Two BRD_I2C addresses are held by kernel drivers on the Linux
image:** `0x70` (`gpio-gd32-bridge`) and `0x52` (`rtc-rv3028`, see
[Real-time clock](#real-time-clock) above). A standard userspace
`i2c-tools` transaction against either address is refused because the
kernel already owns it -- use `i2c -f` (force) from userspace, or go
through the owning kernel driver, rather than probing those two
addresses directly.

Wire spec: [`docs/gd32-bridge-protocol.md`](../gd32-bridge-protocol.md).
Firmware tree: [`docs/gd32-bridge.md`](../gd32-bridge.md).
Host driver: [`<alp/chips/gd32g553.h>`](../../include/alp/chips/gd32g553.h).
Example: [`examples/v2n/v2n-gd32-bridge-ping/`](../../examples/v2n/v2n-gd32-bridge-ping/).

## Power tree

Two PMICs cooperate to bring V2N up:

1. **ACT88760** (primary) -- the hardware-driven CMI 120.E1 power
   sequence brings up the Renesas core + IO rails without firmware
   intervention.  Host firmware just polls status for telemetry.
2. **DA9292** (secondary) -- CH1 is the 0.8 V Renesas core rail
   (strap-enabled at boot).  CH2 is **disabled on V2N base**;
   only V2N-M1 firmware brings it up (DEEPX rail).

**Early-OTP ACT88760 units hold the GD32 in reset.** Some units carry an
early ACT88760 OTP revision that drives GPIO register `0x10` up as `0x88`
at power-on; that GPIO4 bit is wired to `GD32_NRST`, so the GD32 supervisor
never comes out of reset. U-Boot patch 0011 clears bit 7 (`0x10`:
`0x88` -> `0x08`) in `board_late_init()` on every boot, before the GD32
bridge is probed, and prints `ALP: ACT88760 GD32_NRST released (0x10: 0x88
-> 0x08)`. This is a workaround for the early OTP, not a defect fix --
production units carry the fixed OTP, already read `0x08` at power-on, and
the same step is a no-op there (prints `ALP: ACT88760 GD32_NRST already
released`).

### Runtime readings and guarded control

The three drivers (`<alp/chips/act8760.h>`, `<alp/chips/da9292.h>`,
`<alp/chips/tps628640.h>`) read every rail, status bit and ACT88760
GPIO at any time.  **Every control write is fail-closed:** with no limits
table installed it returns `ALP_ERR_NOSUPPORT`.  The tables come from
`metadata/e1m_modules/v2n/power-tree.yaml` (net names, targets, windows,
critical flags, per-boot-mode owners), generated into
`<alp/chips/v2n_power_tree.h>` (`V2N_POWER_*` for V2N, `V2N_M1_POWER_*`
for V2N-M1), and installed with `act8760_set_limits()` /
`da9292_set_limits()` / `tps628640_set_limits()`.  With a table installed:

- a voltage write must encode inside the rail's window (default target
  +/-5 %, rounded inward to the chip's step) and is read back;
- a `critical` rail (every ACT88760 rail, DA9292 CH1, TPS628640 `0x4D`)
  can never be disabled by software;
- the ACT88760 raw write path reaches only MSTR `0x01`, `0x05`,
  `0x2B`, `0x33`; `0x07` (MR / SLEEP / DPSLP / POWER OFF / watchdog),
  `0x09`, `0x0A`, the IO-delay / WDTIME registers `0x0B` / `0x0C`, `0x14`
  (POK_OV / VSYSWARN thresholds -- a bad value can trip PMIC shutdown),
  the factory ranges `0x15`-`0x26` and `0x2D`-`0x32`, every MODEx, every
  tile register and all of ADD2 are refused;
- the only ACT88760 GPIO polarity that may be written is GPIO4
  `GD32_NRST` (MODE4 `0x10`: some units' OTP reads `0x88`, holding the
  GD32 in reset; the volatile fix is `0x08`).

The DEEPX DA9292 CH2 sequence is `da9292_ch2_sequence()`, run by U-Boot
(`board_late_init()`) in `a55_boot` mode; afterwards CA55/Linux is the sole
RIIC8 master (see above).  Boot-mode ownership is recorded in
`metadata/e1m_modules/v2n/power-tree.yaml` (`boot_modes:`); `cm33_boot` is
blocked there.
Bench tool: [`examples/v2n/v2n-pmic-inspect/`](../../examples/v2n/v2n-pmic-inspect/).

## Boot + identification

SoM identification is EEPROM-authoritative:

**EEPROM manifest** -- 128-byte block at offset 0 of the on-module
24C128 carrying family / SKU / hw_rev / serial / mfg date, integrity-
checked (magic + schema + CRC32). Read via `alp_hw_info_read()`. A
blank module returns `ALP_ERR_NOT_PROVISIONED`; a corrupt one returns
`ALP_ERR_IO`. The EEPROM is the sole source of the SoM revision (no
ADC cross-check).

U-Boot validates the same manifest at boot and publishes its SKU to the
kernel as `/chosen/alp,sku`; the `alp-hostname` unit in every ALP image
turns it into the hostname. U-Boot patch 0010 also publishes the unit
serial as `/chosen/alp,serial`, which the unit appends so each board gets
its own name (for example `e1m-v2m103-2026w38-0001`; the whole serial,
since its index restarts every ISO week). The root shell prompt does not
show the serial (`root@e1m-v2m103:~#`: `/etc/profile.d/alp-prompt.sh`
strips it from the hostname for interactive shells), and the pre-login
banner gains a `Module: <SKU>  Serial: <serial>` line: the same unit writes
`/run/alp-module.issue` and the image links `/etc/issue.d/10-alp-module.issue`
to it (agetty appends `/etc/issue.d/*.issue` to `/etc/issue`; the line is
absent on a blank module). The hostname itself is unchanged. An unprovisioned
module, or a bootloader older than u-boot patch 0009, publishes nothing
and keeps the distro default hostname `alp-e1m`.

Full procedure: [`docs/board-id.md`](../board-id.md).
Example: [`examples/v2n/v2n-board-id-readout/`](../../examples/v2n/v2n-board-id-readout/).

### Ethernet MAC address policy {#ethernet-mac-address-policy}

Neither the RZ/V2N SoC nor this SoM has a MAC-address source: the SoC has no
MAC OTP, and there is no MAC EEPROM on the module. Left alone, both `end0`
and `end1` would boot with whatever compiled-in default the enabled BSP
feature layers happen to bake into U-Boot's environment -- identical on
every unit, and (depending on which layers are enabled) not even a real
IEEE-registered address.

U-Boot instead derives `ethaddr`/`eth1addr` at boot from the unit serial
already in the validated identity-EEPROM manifest (`docs/board-id.md`) --
an **injective encoding, not a hash**, into IEEE 802c SLAP
locally-administered space. This makes both MACs **fleet-unique by
construction** (unique across every unit this allocator has issued a serial
to) -- **not globally unique** the way a purchased IEEE OUI block would
make them; that block is not something the project has bought.

The MAC carries no SKU field, so the serial's index is allocated
**fleet-wide per ISO week, across every SKU** -- not per SKU -- which is
what makes the encoding actually unique: two different SKUs sharing a
week and an index would otherwise collide on the same MAC.

* Layout: octet 0 fixed `0xA2` (individual, locally administered), then a
  40-bit payload: 4-bit Alp Lab prefix `0xC` | 6-bit `year - 2024` | 6-bit
  ISO week | 20-bit Crockford base32 index (the serial's `NNNN`/`IIII`
  field, alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ` -- no `I`/`L`/`O`/`U`,
  never aliased) | 2-bit interface (`0` = `end0`, `1` = `end1`) | 2 reserved
  bits.
* Single source of the values above: `metadata/identity/serial-mac.json`
  (#2361); the Python side loads it and the tests pin the C side to it.
* Canonical implementation: `scripts/alp_eth_mac.py` (host/tooling side) and
  U-Boot patch `meta-alp-sdk/recipes-bsp/u-boot/u-boot/0010-rzv2n-dev-ALP-E1M-serial-derived-eth-mac.patch`
  (device side) -- the two must stay bit-for-bit identical; both carry the
  same golden vector for serial `2026W38-0001`: `end0 = A2:C0:A6:00:00:10`,
  `end1 = A2:C0:A6:00:00:14`.
* **Runs twice, on purpose:** once from `board_late_init()` (so U-Boot's
  own networking has a MAC before `bootcmd` runs), and again from a new
  `alp_eth_mac` command that `CONFIG_BOOTCOMMAND` invokes immediately
  after `env default -a`. The second call is the one that actually
  reaches Linux: U-Boot's `image_setup_libfdt()` runs
  `fdt_fixup_ethernet()` (which copies `ethaddr`/`eth1addr` into the
  `ethernet0`/`ethernet1` DT nodes' `mac-address`/`local-mac-address`
  properties) *before* `ft_system_setup()` ever runs. Deriving only in
  `board_late_init()` would leave nothing for Linux to see: its value is
  *wiped* by that same `env default -a` a few bootcmd tokens later.
  Deriving only in `ft_system_setup()` would already be too late for
  that same boot instead, since `fdt_fixup_ethernet()` has already run
  by the time it executes.
* **No env override, by construction, not by choice:** `CONFIG_BOOTCOMMAND`
  opens with `env default -a`, which wipes the whole environment back to
  its compiled-in defaults on every boot before Linux is reached -- so a
  `setenv ethaddr <mac>; saveenv` would not survive to the next autoboot
  regardless of what U-Boot's derivation code did. The honest rule this
  SoM ships is: **the derived MAC always applies whenever the serial
  parses**, unconditionally, every boot. There is no override slot --
  **conditional on autoboot actually running the compiled-in
  `CONFIG_BOOTCOMMAND`.** A unit carrying a *saved* `bootcmd` from an
  older FIP (predating this `alp_eth_mac` command) or from a manual
  `setenv bootcmd; saveenv` runs THAT saved command instead -- one with
  no `alp_eth_mac` call. Its `env default -a` (still present in every
  version of this bootcmd) then wipes whatever `board_late_init()` set,
  and Linux sees the DRP-AI vendor default `02:11:22:33:44:55`/`66`
  instead of the derived MAC. See `docs/provisioning.md`'s FIP-flash
  step for the saved-env reset a FIP upgrade on a previously-provisioned
  unit must carry.
* **Unprovisioned EEPROM (no serial, or one that does not parse):**
  U-Boot prints `ALP: WARNING: ... SoM EEPROM not provisioned` and
  derives the MACs from the on-module eMMC's CID instead: CRC-32 of the
  128-bit CID in the same layout under prefix nibble `0xD` (serial-derived
  MACs use `0xC`, so the two families never overlap). That keeps
  unprovisioned units off the shared vendor default, but it is a hash, not
  an identity -- two such units collide with odds of about n^2 / 2^33, and
  the MAC changes if the eMMC is replaced. Provision the EEPROM before a
  unit ships. Only a unit with no readable eMMC either keeps the vendor
  default, with a second WARNING line.
* Recompute a unit's MAC any time from its serial with
  `scripts/alp_eth_mac.py 2026W38-0001` (no ledger field carries it --
  it is cheap to recompute and would otherwise just be a value that can
  drift from the encoding that produces it).

Octet 0 `0xA2` has U/L=1, I/G=0 and IEEE 802c-2017 SLAP quadrant bits
Z:Y=`00`, i.e. the *Administratively Assigned Identifier* (AAI) quadrant --
the range a local administrator may assign without buying an IEEE block.

### SoC OTP (not used by the SDK) {#soc-otp}

The RZ/V2N carries one 32-Kbit OTP unit (`"otp": 1` in
`metadata/socs/renesas/rzv2n/n44.json`; datasheet R01DS0466EJ0120 and
hardware manual R01UH1071EJ0120 Rev.1.20, section 4.10), supplied from
`OTPVDD18`. Base address `0x10450000` (CM33 view: `0x50450000`
non-secure, `0x40450000` secure). Writes go in 16-bit units, reads in
32-bit units, and each bit can be written once. The unit is addressed
by the manual's Table 4.10-3 area map:

| Area | OTP address |
|---|---|
| Chip product ID (individual identification) | `0F3h` to `0F6h` |
| One-time read area enable setting | `12Ah` |
| Boot device drive strength setting | `12Ch` |
| User area 1 (one-time read area) | `160h` to `1DFh` |
| User area 2 | `1E0h` to `3DFh` |

The SDK does not read or write it: identity lives in the EEPROM manifest
above, and there is no MAC area (see the MAC policy). **Writing it is
permanent** -- a wrong boot-device drive-strength value can stop the SoC
booting -- so treat it as out of scope for provisioning unless a later
decision puts something there.

## Wi-Fi + Bluetooth (Linux)

Every V2N/V2M SKU carries the same on-module Murata LBEE5HY2FY-922
(Infineon CYW55513), Wi-Fi 6/6E 1x1 HE20 tri-band + BT 5.4, on SDHI2
(4-bit SDIO) + RSCI4 (BT UART). Both REG_ON enables are GD32 bridge
GPIO lines -- **not** SoC pins -- and only exist on bridge firmware
>= 0.2.12 / protocol minor 11 (`GD32G553_REG_ON_MIN_PROTOCOL_MINOR`, see
[`include/alp/chips/gd32g553.h`](../../include/alp/chips/gd32g553.h)):

| Signal    | GD32 pad | Bridge GPIO line |
|-----------|----------|-------------------|
| WL_REG_ON | PE15     | 19                 |
| BT_REG_ON | PE14     | 18                 |

The **Linux** side lives entirely in `meta-alp-sdk` (the Zephyr/M33
side has no Wi-Fi role):

* `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi` --
  `&sdhi2` (WLAN, `mmc-pwrseq-simple` on line 19), `&sci4` (BT,
  `brcm,bcm43438-bt` `shutdown-gpios` on line 18), the `sd2_wlan_pins` /
  `sci4_bt_pins` pinctrl groups.
* `meta-alp-sdk/recipes-kernel/linux/linux-renesas/wifi-bt.cfg` --
  `CONFIG_CFG80211=m` + in-tree `CONFIG_BRCMFMAC` off (no CYW55513 ID
  in the 6.1.x in-tree driver) + the Bluetooth HCI UART/Broadcom stack
  as modules + `CONFIG_GPIO_GD32_BRIDGE=y`. The BT stack must stay `=m`:
  built in, `hci_uart_bcm` probes before the root filesystem is mounted,
  cannot load `brcm/BCM.hcd`, and hci0 setup times out on opcode 0x1003.
* `meta-alp-sdk/recipes-kernel/cyw-fmac/` -- the out-of-tree Murata
  `cyw-fmac` backports kmod (`compat`/`cfg80211`/`brcmutil`/`brcmfmac`,
  installed under `updates/` so depmod prefers it over any in-tree
  module).
* `meta-alp-sdk/recipes-kernel/cyw-fmac-firmware/` -- the five
  firmware blobs (WLAN `.trxse`, CLM, NVRAM, two regional BT `.hcd`
  patches) from four upstream `murata-wireless` / `Infineon` repos,
  pinned by `SRCREV`.
* `wireless-regdb-static`, `iw`, `wpa-supplicant`, `bluez5` -- standard
  OE-core recipes, pulled in by `alp-image-common.inc` for any V2N/V2M
  `MACHINE_FEATURES`.

**Bench results (2026-09-26, E1M-V2M103, cyw-fmac fw 28.10.387.10,
kernel 6.1.141-cip43):**

* Wi-Fi scan, WPA2/SAE association on 5 GHz channel 60, and DHCP all
  worked end to end.
* `sd-uhs-sdr50` on `&sdhi2` (above) negotiates SDR50 at 100 MHz (up
  from HS at 50 MHz); `sd-uhs-sdr104` is deliberately not set --
  untested 208 MHz tuning on this non-removable module is not worth
  the risk yet.
* `brcm,ccode-map-trivial` on the `wifi@1` node lets `iw reg set DE`
  reach the firmware; confirmed with `wireless-regdb-static` installed.
* Open HW note: bench RSSI on the E1M-V2M103 EVK reads roughly 35 dB
  below a phone at the same spot -- points at that unit's antenna/RF
  path, under investigation, not a software issue.

**Bench TODO:** the E1M-X-EVK carrier dtsi (`e1m-x-evk.dtsi`) no longer
parks PB0/PB1 as usb30 VBUS/OVC GPIOs -- those SoC pins are the
on-module WLAN group's `SD2CLK`/`SD2DAT0`, never routed to the E1M
connector (the old `usb30_pins`/`usb-ovc-disable-hog` pair was copied
from the Renesas EVK reference dts without checking the ALP module's
netlist). usb30 OC processing is left at its controller default;
verify on the bench that xHCI reports no spurious over-current with
PB1 now muxed as `SD2DAT0` -- if it does, suppress usb30 OC at the
controller the same way usb20 is suppressed (see `&ehci0`'s comment in
`e1m-x-evk.dtsi`).

**Bench history**: the 2026-06 WLAN bring-up on this exact node shape
chased pull-ups, JTAG_SEL, IOVS pad-voltage mode, and the SDIO/gSPI
boot strap before converging on the on-module 32.768 kHz LPO (sourced
from the on-module 5L35023B clock generator) as the real blocker --
tracked separately in #2293, not part of this change. The 2026-09-26
bench results above are the re-run against this node shape.
BT (raw HCI, manual REG_ON) was bench-confirmed working at 115200 baud
on `/dev/ttySC4` in 2026-06; the serdev/`shutdown-gpios` path above
replaces that manual toggle and was re-run on silicon 2026-09-26
(E1M-V2M103): `hci0` UP+RUNNING, BD_ADDR read via HCIGETDEVINFO.
Re-run 2026-09-27 with the BT stack as modules: `bluetooth`/`hci_uart`/`btbcm`
autoload after rootfs, the `brcm/BCM.hcd` patch loads (chip id 157), and
`hci0` comes UP+RUNNING.

## Linux UART ports (SCIF)

The on-module RZ/V2N SCIF UARTs enumerate as `/dev/ttySC<N>` (console on
`ttySC0`, Bluetooth HCI on `ttySC4`). `alp_uart_open()` reaches them with
`port_id = 300 + N` (300..399 -> `/dev/ttySC<N>`), so no hand-written tty
wrapper is needed: `alp_uart_config_t cfg = ALP_UART_CONFIG_DEFAULT(300u + 1u);`
opens `/dev/ttySC1`. Don't open a port the kernel already owns (the console or
the BT UART).

## Bring-up

Step-by-step bench bring-up: [`docs/bring-up-v2n.md`](../bring-up-v2n.md).
Covers first-power smoke test, SWD attach + GD32 firmware flash,
host-to-bridge link confirmation, SoM manifest read, dual
Ethernet bring-up, on-module fleet sanity checks.

## Pins

* `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` -- Renesas
  RZ/V2N pad → E1M peripheral function.
* `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` -- GD32 pad → E1M
  peripheral function.

Both files are tab-delimited; consume directly or via
`scripts/alp_project.py`.

## Example apps targeting V2N

| Example                          | What you'll see                                             |
|----------------------------------|-------------------------------------------------------------|
| `v2n-gd32-bridge-ping`           | Round-trip PING + GET_VERSION on both transports.           |
| `v2n-board-id-readout`           | SoM EEPROM manifest read + SKU assertion.                   |
| `v2n-ethernet-dual`              | Bring up both RTL8211FDI PHYs (ET0 + ET1); WoL configuration.|
| `v2n-eeprom-manifest-dump`       | Hexdump + decode the 128-byte EEPROM manifest.              |
| `v2n-temp-sensor`                | TMP112 read loop -- classic starter app.                    |
| `v2n-pwm-fan-control`            | Ramp a GD32-side PWM channel along a five-stop fan curve.   |
| `v2n-secure-element-sign`        | OPTIGA Trust M probe, Coprocessor UID read and raw APDU session (host library). |
| `v2n-xspi-flash-readwrite`       | Erase + write + verify one page on the on-module xSPI NOR.  |
| `v2n-emmc-block-stat`            | Read on-module eMMC geometry + first block via disk-access. |
| `v2n-gd32-swd-flash`             | Host-driven SWD bit-bang -- IDCODE read, halt, erase/write/verify, reset. |

Plus every cross-family example
(`gpio-button-led`, `i2c-scanner`, `pwm-led-fade`, `rtc-clock`, …).
See [`examples/README.md`](../../examples/README.md).

## Common gotchas

| Symptom                                       | Cause + fix                                                                                  |
|-----------------------------------------------|----------------------------------------------------------------------------------------------|
| Boot console silent                           | Check the primary PMIC's `nRESET` -- should release within a few ms of `V_IN`. See [`docs/troubleshooting.md`](../troubleshooting.md). |
| Ethernet PHY won't ACK on MDIO                | 1.8 V rail not up, or the 1 kΩ pull-ups missing.                                              |
| `gd32g553_init` returns `ALP_ERR_NOSUPPORT`   | Firmware major version mismatch; reflash bridge firmware from matching commit.               |
| `da9292_ch2_sequence` -> NOSUPPORT            | V2N base: the CH2 limits entry is all-zero (no DEEPX load), so the sequence is refused.       |
| Ethernet PHY ID reads `0x0000`                | Wrong PHY address; check strap on schematic (default `0x00` after reset).                     |
| SoC RTC (`&rtc`) fails to probe, `-ETIMEDOUT` | The on-module clock-generator fixup didn't apply (old/bypassed U-Boot). See [On-module clock-generator fixup](#on-module-clock-generator-fixup) above. |

Full list: [`docs/troubleshooting.md`](../troubleshooting.md).

## See also

* [`v2n-m1.md`](v2n-m1.md) -- the AI-accelerator variant.
* [`aen.md`](aen.md) -- the smaller Alif Ensemble form factor.
* [`imx93.md`](imx93.md) -- the NXP i.MX 93 family.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
