# E1M-X V2N family

> Renesas RZ/V2N-based SoMs in the E1M-X (45 × 65 mm) form factor.

This page is the single landing point for firmware engineers
working with the V2N module.  Skim it once, then follow the
deep-link of whatever you're doing.

## SKUs

| SKU            | Memory                                | Status     |
|----------------|---------------------------------------|------------|
| `E1M-V2N101`   | 32 Gbit LPDDR4X + 32 Gbit eMMC        | production |
| `E1M-V2N102`   | 64 Gbit LPDDR4X + 64 Gbit eMMC        | production |

Both SKUs share the same silicon + PCB.  Pick by memory budget.

## What's on the module

| Role                    | Part                       | Bus / signal     | Driver                                  |
|-------------------------|----------------------------|------------------|-----------------------------------------|
| Application SoC         | Renesas RZ/V2N (R9A09G056N44) | -- | (vendor HAL)                                  |
| Companion supervisor MCU| GigaDevice GD32G553MEY7TR  | SPI + I2C bridge | [`<alp/chips/gd32g553.h>`](../../include/alp/chips/gd32g553.h) |
| Primary PMIC            | Qorvo ACT88760-120.E1      | I2C `0x25/0x26`  | [`<alp/chips/act8760.h>`](../../include/alp/chips/act8760.h) |
| Secondary PMIC          | Renesas DA9292             | I2C `0x1C`       | [`<alp/chips/da9292.h>`](../../include/alp/chips/da9292.h) |
| Optional buck (LPDDR4X) | TI TPS628640 (1×, optional)| I2C `0x4D`       | [`<alp/chips/tps628640.h>`](../../include/alp/chips/tps628640.h) |
| Clock generator         | Renesas / IDT 5L35023B     | I2C `0x68`       | [`<alp/chips/clk_5l35023b.h>`](../../include/alp/chips/clk_5l35023b.h) |
| RTC                     | Micro Crystal RV-3028-C7   | I2C `0x52`       | [`<alp/chips/rv3028c7.h>`](../../include/alp/chips/rv3028c7.h) |
| Temperature sensor      | TI TMP112                  | I2C `0x40`       | [`<alp/chips/tmp112.h>`](../../include/alp/chips/tmp112.h) |
| Secure element          | Infineon OPTIGA Trust M    | I2C `0x30`       | [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h) |
| EEPROM (SoM manifest)   | Onsemi N24S128             | I2C `0x50` (E1M_I2C0) | [`<alp/chips/eeprom_24c128.h>`](../../include/alp/chips/eeprom_24c128.h) |
| Wi-Fi 6 + BLE 5.4       | Murata LBEE5HY2FY-922      | SDIO + UART + I2S | [`<alp/chips/murata_lbee5hy2fy.h>`](../../include/alp/chips/murata_lbee5hy2fy.h) |
| Ethernet PHY 0          | Realtek RTL8211FDI-VD-CG   | RGMII + MDIO     | [`<alp/chips/rtl8211fdi.h>`](../../include/alp/chips/rtl8211fdi.h) |
| Ethernet PHY 1          | Realtek RTL8211FDI-VD-CG   | RGMII + MDIO     | (same driver, second instance)          |
| eMMC                    | (variant per SKU)          | Renesas SD0      | Zephyr SD subsystem                     |
| NOR flash               | (variant per SKU)          | Renesas xSPI0    | Zephyr flash subsystem                  |

Full chip catalogue + manifest URLs:
[`metadata/chips/`](../../metadata/chips/).
Per-SKU populated parts: [`metadata/e1m_modules/E1M-V2N10{1,2}.yaml`](../../metadata/e1m_modules/).

## Reach the GD32 supervisor

The V2N's GD32G553 supervisor MCU owns half the E1M-edge
peripherals (eight PWM channels, dual ADC + DAC bank, the Wi-Fi/BT
REG_ON pins, OPTIGA reset, 18 IO routes to the E1M edge).  The
host driver speaks both transports:

* **SPI fast path** -- Renesas RSPI master on `P76/P77/P96/P97`
  ↔ GD32 slave on `PA8/9/10/PB15`.  Use for high-frequency
  telemetry + PWM updates.
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

Full procedure: [`docs/board-id.md`](../board-id.md).
Example: [`examples/v2n/v2n-board-id-readout/`](../../examples/v2n/v2n-board-id-readout/).

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
| `v2n-rtc-multi-alarm`            | Multi-source callbacks on the rv3028c7 dispatcher.          |
| `v2n-temp-sensor`                | TMP112 read loop -- classic starter app.                    |
| `v2n-pwm-fan-control`            | Ramp a GD32-side PWM channel along a five-stop fan curve.   |
| `v2n-secure-element-sign`        | OPTIGA Trust M init + ECDSA-P256 sign via raw APDU.         |
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

Full list: [`docs/troubleshooting.md`](../troubleshooting.md).

## See also

* [`v2n-m1.md`](v2n-m1.md) -- the AI-accelerator variant.
* [`aen.md`](aen.md) -- the smaller Alif Ensemble form factor.
* [`imx93.md`](imx93.md) -- the NXP i.MX 93 family.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
