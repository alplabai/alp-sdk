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

## Real-time clock

The on-module RV-3028-C7 is the RTC of record, bound as `/dev/rtc0`
(kernel `rtc-rv3028`, `CONFIG_RTC_DRV_RV3028=y`) -- use `hwclock`/`date`
from userspace. **CA55 (Linux) is the sole master of the whole
RIIC8/BRD_I2C bus** the RTC and every other BRD_I2C device sit on
(`metadata/e1m_modules/v2n/core-ownership.yaml`); the CM33 must never
issue I2C transactions there. No `trickle-resistor-ohms` is configured
(the RV-3028-C7's VBACKUP/backup-cap wiring isn't confirmed on this
SoM's schematic) and the alarm INT line isn't wired to a kernel
interrupt yet -- both are open follow-ups.

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
turns it into the hostname (for example `e1m-v2m103`). An unprovisioned
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

* Layout: octet 0 fixed `0xA2` (individual, locally administered), then a
  40-bit payload: 4-bit Alp Lab prefix `0xC` | 6-bit `year - 2024` | 6-bit
  ISO week | 20-bit Crockford base32 index (the serial's `NNNN`/`IIII`
  field, alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ` -- no `I`/`L`/`O`/`U`,
  never aliased) | 2-bit interface (`0` = `end0`, `1` = `end1`) | 2 reserved
  bits.
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
  properties) *before* `ft_system_setup()` ever runs -- so deriving only
  in `board_late_init()`, or only in `ft_system_setup()`, would already
  be too late for that same boot.
* **No env override, by construction, not by choice:** `CONFIG_BOOTCOMMAND`
  opens with `env default -a`, which wipes the whole environment back to
  its compiled-in defaults on every boot before Linux is reached -- so a
  `setenv ethaddr <mac>; saveenv` would not survive to the next autoboot
  regardless of what U-Boot's derivation code did. The honest rule this
  SoM ships is: **the derived MAC always applies whenever the serial
  parses**, unconditionally, every boot. There is no override slot.
* Recompute a unit's MAC any time from its serial with
  `scripts/alp_eth_mac.py 2026W38-0001` (no ledger field carries it --
  it is cheap to recompute and would otherwise just be a value that can
  drift from the encoding that produces it).

Octet 0 `0xA2` has U/L=1, I/G=0 and IEEE 802c-2017 SLAP quadrant bits
Z:Y=`00`, i.e. the *Administratively Assigned Identifier* (AAI) quadrant --
the range a local administrator may assign without buying an IEEE block.

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
| `v2n-secure-element-sign`        | OPTIGA Trust M I2C_STATE probe; APDU/product-info paths return `ALP_ERR_NOSUPPORT` today. |
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
| `da9292_v2n_m1_enable_deepx_rail` -> TIMEOUT  | This call is V2N-M1 only -- the V2N base SoM doesn't have a DEEPX load.                       |
| Ethernet PHY ID reads `0x0000`                | Wrong PHY address; check strap on schematic (default `0x00` after reset).                     |
| SoC RTC (`&rtc`) fails to probe, `-ETIMEDOUT` | The on-module clock-generator fixup didn't apply (old/bypassed U-Boot). See [On-module clock-generator fixup](#on-module-clock-generator-fixup) above. |

Full list: [`docs/troubleshooting.md`](../troubleshooting.md).

## See also

* [`v2n-m1.md`](v2n-m1.md) -- the AI-accelerator variant.
* [`aen.md`](aen.md) -- the smaller Alif Ensemble form factor.
* [`imx93.md`](imx93.md) -- the NXP i.MX 93 family.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
