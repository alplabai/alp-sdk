# v2n-eeprom-manifest-dump

Hexdumps the 128-byte hardware-info manifest at offset 0x0000 of
the on-module 24C128 EEPROM, decodes every field, and verifies the
CRC.  Useful during bring-up to confirm the production-test fixture
programmed the module correctly.

## What it does

1. Opens the configured I2C bus + 24C128 driver.
2. Reads bytes `[0x00, 0x80)` in a single transaction.
3. Hexdumps the bytes (xxd-style, 16 bytes per line + ASCII gutter).
4. Decodes the magic, schema_version, family, sku, hw_rev, serial,
   manufacturing date.
5. Verifies the CRC-32 ISO-3309 over the first 124 bytes.
6. Also runs `alp_hw_info_read()` so the status surfaces the same
   way it would in production firmware.

## Expected output (correctly-programmed module)

```
[manifest] v2n-eeprom-manifest-dump
[manifest] raw bytes:
  0000  48 50 4c 41 01 00 00 00 76 32 6e 00 00 00 00 00  |HPLA....v2n.....|
  0010  00 00 00 00 00 00 00 00 45 31 4d 2d 56 32 4e 31  |........E1M-V2N1|
  0020  30 31 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |01..............|
  0030  72 31 00 00 00 00 00 00 32 30 32 36 57 31 39 2d  |r1......2026W19-|
  0040  30 30 30 31 00 00 00 00 00 00 00 00 00 00 00 00  |0001............|
  0050  ea 07 05 0b 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0060  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0070  00 00 00 00 00 00 00 00 00 00 00 00 ?? ?? ?? ??  |................|

[manifest] magic         = 0x414c5048  (OK -- ASCII 'ALPH')
[manifest] schema_version= 1  (OK)
[manifest] family        = v2n
[manifest] sku           = E1M-V2N101
[manifest] hw_rev        = r1
[manifest] serial        = 2026W19-0001
[manifest] mfg date      = 2026-05-11
[manifest] crc32         = 0x???????? (stored)  vs 0x???????? (computed)  (OK)

[manifest] alp_hw_info_read() -> status=0
[manifest]   som_family=v2n som_sku=E1M-V2N101 som_hw_rev=r1 som_serial=2026W19-0001
[manifest] done
```

## Expected output (unprogrammed module)

The EEPROM ACKs but the bytes are all `0xFF` (or all `0x00` after
a wipe).  The example reports the magic mismatch + the CRC
mismatch -- flag this module for production-test follow-up before
attempting to deploy firmware to it.

## Adapting

The example's `board.yaml` declares the EEPROM at the V2N default
bus (E1M_I2C0) + address (`0x50`).  Change those values if your
carrier wires the EEPROM elsewhere.

## See also

* [`docs/board-id.md`](../../../docs/board-id.md) -- customer-facing
  spec for the manifest.
* [`<alp/hw_info.h>`](../../../include/alp/hw_info.h) -- `struct
  alp_hw_info_eeprom_t`.
* [`tools/program_eeprom.py`](../../../tools/program_eeprom.py) --
  the production-test programmer that writes the manifest.
