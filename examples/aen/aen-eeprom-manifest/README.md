# aen-eeprom-manifest

Read + decode the 128-byte Alp hardware-info manifest from the on-module **24C128
EEPROM** on the E1M-AEN (Alif Ensemble) SoM, over the portable `<alp/*>` API.
The AEN sibling of [`v2n-eeprom-manifest-dump`](../../v2n/v2n-eeprom-manifest-dump)
(same `src/` flow).

## The bus

On the E1M-AEN801 the EEPROM's interface is selected by **bridge/DNP resistors**
onto **SoC I2C2** — a Synopsys DesignWare master bus (pins `P5_6 SCL_C` /
`P5_7 SDA_C`), driven by **upstream Zephyr's `i2c_dw`** (full master read+write).
It is **not** on the slave-only LPI2C0 (which carries the RTC/TMP112 in this
board rev; a respin moves those to a master bus). This is Tier-1 upstream-native
per [ADR 0017](../../../docs/adr/0017-alp-sdk-over-the-vendor-sdk.md) — no vendor
driver, just the DT node (`i2c2@49012000`, authoritative `I2C2_BASE`/`I2C2_IRQ`
from the AE822 DFP device header) + the portable backend.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-eeprom-manifest
west flash
```

The board overlay (`boards/alp_e1m_aen801_m55_he.overlay`) enables `i2c2`,
supplies `pinctrl_i2c2` (P5_6/P5_7), and aliases `alp-i2c0 → &i2c2`.

## What it shows

1. `alp_i2c_open(bus 0)` → SoC I2C2; `eeprom_24c128_init(0x50)`.
2. Read the full 128-byte manifest (single write-then-read / repeated-START).
3. Hex-dump + decode every field (magic `ALPH`, schema, family, SKU, hw_rev,
   serial, mfg date) + verify the CRC-32 against the stored value.

Expected on a programmed module: `magic ... (OK)`, the SKU/serial/date, `crc32 ...
(OK)`. An erased/unprogrammed module fails the magic check.

> **BENCH-VERIFIED (RESULT PASS).** On the E8 the populated bridge/DNP routes the
> EEPROM to I2C2 (the 24C128 reads back at `0x50`, one of 12 devices on the bus).
