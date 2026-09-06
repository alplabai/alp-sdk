# aen-eeprom-manifest

Read + decode the 128-byte Alp hardware-info manifest from the on-module **24C128
EEPROM** on the E1M-AEN (Alif Ensemble) SoM, over the portable `<alp/*>` API.
The AEN sibling of [`v2n-eeprom-manifest-dump`](../../v2n/v2n-eeprom-manifest-dump)
(same `src/` flow).

## The bus

On the E1M-AEN801 the EEPROM's interface is selected by **bridge/DNP resistors**
onto **SoC I2C2** — a Synopsys DesignWare master bus (pins `P5_6 SCL_C` /
`P5_7 SDA_C`), driven by **upstream Zephyr's `i2c_dw`** (full master read+write).
It is **not** on BRD_I2C (SoC I2C0, which carries the RTC/TMP112/OPTIGA
instead -- see `examples/aen/aen-secure-element-sign`). This is Tier-1 upstream-native
per [ADR 0017](../../../docs/adr/0017-alp-sdk-over-the-vendor-sdk.md) — no vendor
driver, just the DT node (`i2c2@49012000`, authoritative `I2C2_BASE`/`I2C2_IRQ`
from the AE822 DFP device header) + the portable backend.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-eeprom-manifest
west flash
```

The board overlay
(`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`) enables
`i2c2`, supplies `pinctrl_i2c2` (P5_6/P5_7), and aliases `alp-i2c0 → &i2c2`.
Zephyr only auto-applies a `boards/<name>.overlay` when `<name>` matches the
build's fully-qualified board id, so the overlay filename must be the FQ form
above -- a bare `alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` is silently dropped by the
`west build` command above.

This app links at the HE MRAM app partition (`CONFIG_FLASH_LOAD_OFFSET=0x10000`
in `prj.conf`, reset vector `0x8001xxxx`) and boots as a normal SES/slot0
flash target, the same mechanism the `aen-cc3501e-*` examples use -- it is
not an ITCM RAM-run.

## What it shows

1. `alp_i2c_open(bus 0)` → SoC I2C2; `eeprom_24c128_init(0x50)`.
2. Read the full 128-byte manifest (single write-then-read / repeated-START).
3. Hex-dump + decode every field (magic `ALPH`, schema, family, SKU, hw_rev,
   serial, mfg date) + verify the CRC-32 against the stored value.
4. Call `eeprom_24c128_read_identity()` and print the second device-select
   header's four objects (Secure Data Page, Unique ID, Lock Status, Device
   Configuration Register).

Expected on a programmed module: `magic ... (OK)`, the SKU/serial/date, `crc32 ...
(OK)`. An erased/unprogrammed module fails the magic check.

### The `0x50` / `0x58` second device-select header

The on-module EEPROM answers at **both** `0x50` and `0x58` — this is **one part**
(onsemi **N24S128**), not two chips. `0x50` is device-select header `1010` plus
the strapped A2/A1/A0; `0x58` is header `1011` plus the *same* straps
(`0x50` + `EEPROM_24C128_ALT_ADDR_OFFSET`, i.e. `+ 0x08`). The `1011` space
exposes four **read-only** objects, selected by the first pointer byte:
the 64-byte Secure Data Page (`0x00`), the 16-byte Unique ID (`0x02`), the
1-byte Lock Status (`0x04`), and the 1-byte Device Configuration Register
(`0x06`). `eeprom_24c128_read_identity()` reads all four in one call.

A per-object `_valid` flag reading false, with the call itself still returning
`ALP_OK`, is **not a failure** — it is the expected result on a board populated
with the footprint-compatible alternate part, **M24128-BFMH6TG**, which has no
second device-select header to answer at all. This example prints `NACK
(expected on the M24128-BFMH6TG alternate ...)` in that case rather than
failing, and none of the four identity checks feed the `RESULT PASS`/`RESULT
FAIL` verdict — that verdict still gates only on the manifest's magic/schema/
CRC-32, so the bench's grep for `RESULT PASS:` / `RESULT FAIL:` is unaffected.

This example is deliberately **read-only** and never calls
`eeprom_24c128_write()`: a stray write at selector `0x06` lands in the Device
Configuration Register, whose low bits are the device's own strapped A2/A1/A0
(a write there would move the EEPROM off `0x50`), and whose SWP bit
permanently write-protects the array, the Secure Data Page, and the register
together.

### Sample output (E1M-AEN803, serial 2026W36-0003, SoC I2C2, 2026-09-06)

```
[manifest]   Secure Data Page (64 bytes, erased = all 0xff):
  0000  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff  |................|
  0010  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff  |................|
  0020  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff  |................|
  0030  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff  |................|
[manifest]   Unique ID (factory-set, not uniform-random -- a trailing run is a printable lot code):
  0000  3c 74 00 24 00 7f 07 57 37 33 32 31 32 36 31 00  |<t.$...W7321261.|
[manifest]   Secure Page Lock Status : UNLOCKED
[manifest]   Device Configuration Reg: 0x1d
```

> **BENCH-VERIFIED (RESULT PASS).** On the E8 the populated bridge/DNP routes the
> EEPROM to I2C2 (the 24C128 reads back at `0x50`, one of 12 devices on the bus;
> the same part also answers its second device-select header at `0x58`). All
> four identity objects above answered on this unit; the `NACK` / alternate-part
> path is documented but untested on this bench.
