<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 13: EEPROM manifest provisioning

**Target audience:** production-test technicians + firmware
engineers responsible for the per-device serial / hw_rev / mfg
date programming flow.

**Prerequisites:**

- A populated E1M module with an unprogrammed 24C128 EEPROM
  (factory state).
- The board exposing BRD_I²C with a suitable USB-I²C adapter
  (Aardvark, MCP2221, FT232H).
- Python 3.10+, `pip install pyyaml`.

**Outcome:** write the 128-byte Alp manifest into the on-module
24C128 EEPROM.  Confirm read-back from runtime via
`<alp/hw_info.h>`.  Understand the BOARD_ID ADC companion path.

**Time:** 5 minutes per device after the first one's flow is
calibrated.

---

## What the manifest carries

128 bytes at EEPROM offset `0x0000`.  Layout (see
[`include/alp/hw_info.h`](../../include/alp/hw_info.h) for the
authoritative C struct):

```
offset  size  field        description
─────────────────────────────────────────────────────────────
   0     4    magic         'ALPH' (0x41 0x4C 0x50 0x48)
   4     1    schema_v1     0x01
   5     4    family        ASCII zero-padded ("AEN_", "V2N_", ...)
   9    16    sku           ASCII zero-padded ("E1M-AEN801")
  25     8    hw_rev        ASCII zero-padded ("r1")
  33    16    serial        ASCII; production-assigned
  49     8    mfg_date      BCD: YYYYMMDD
  57    67    reserved      zero-padded
 124     4    crc32_le      CRC-32 (ISO-3309) over bytes 0..123
─────────────────────────────────────────────────────────────
```

The CRC ensures a half-written manifest is caught at boot --
the SDK's `<alp/hw_info.h>` reader rejects any manifest whose
CRC doesn't match.

## 1. Stand up your production-test rig

Hardware:

- USB-I²C adapter wired to the board's BRD_I²C test points
  (SCL + SDA + GND).  Standard 1.8 V or 3.3 V level shifter on
  the adapter, matching the SoM family.
- Board in factory-test mode (no application running --
  either powered through the USB-I²C alone, or running a
  factory-test firmware that gives I²C bus access to the
  external master).

Software:

- Python 3.10+
- The `scripts/program_eeprom.py` tool from this repo.

Test the bus:

```bash
# Linux + Aardvark:
sudo apt install python3-pip
pip3 install aardvark-py
python3 -c "from aardvark_py import *; print(aa_find_devices(1))"
# Should print: [1, [(0, 0x40, 0x0)]] for one Aardvark adapter.
```

## 2. Build the per-device manifest data

Per-device inputs:

- **family** (`AEN_`, `V2N_`, `V2M_`, `NX9_`) -- from the SKU.
- **sku** (`E1M-AEN801`) -- the SoM MPN.
- **hw_rev** (`r1`, `r2`, ...) -- the PCB revision.
- **serial** (`A20260514-0001`) -- production-assigned;
  recommend a date prefix + sequence number.
- **mfg_date** (`20260514`) -- the calendar date the unit
  was tested.

CRC32 is computed by the script.

## 3. Run the programmer

```bash
cd ~/work/alp-sdk

python3 scripts/program_eeprom.py \
    --sku        E1M-AEN801 \
    --hw_rev     r2 \
    --serial     A20260514-0001 \
    --mfg_date   2026-05-14 \
    --i2c-adapter aardvark0 \
    --i2c-addr   0x50 \
    --eeprom-offset 0x0000
```

Expected output:

```
[program_eeprom] resolved family from SKU: AEN_
[program_eeprom] manifest layout:
  magic       : 0x41 0x4C 0x50 0x48  ('ALPH')
  schema_v1   : 0x01
  family      : 'AEN_'
  sku         : 'E1M-AEN801'
  hw_rev      : 'r2'
  serial      : 'A20260514-0001'
  mfg_date    : 2026-05-14 (BCD 20260514)
  crc32       : 0xC4B2A1E0 (LE)
[program_eeprom] writing 128 bytes to /dev/i2c-1 0x50 offset 0x0000
[program_eeprom] read-back verify: OK
[program_eeprom] Done.
```

The script writes the 128 bytes, reads them back, and verifies
byte-for-byte equality.  Any mismatch aborts before the success
message.

## 4. Verify from device-side firmware

Build + flash any example.  In the application, read the
manifest:

```c
#include "alp/hw_info.h"

int main(void) {
    alp_hw_info_t info;
    if (alp_hw_info_read(&info) != ALP_OK) {
        printf("hw_info: read failed last_err=%d\n",
               (int)alp_last_error());
        return 1;
    }

    printf("[hw_info] family=%s sku=%s hw_rev=%s\n",
           info.som_family, info.som_sku, info.som_hw_rev);
    printf("[hw_info] serial=%s mfg_date=%04u-%02u-%02u\n",
           info.som_serial,
           info.som_mfg_year,
           info.som_mfg_month,
           info.som_mfg_day);
    return 0;
}
```

Expected on the UART:

```
[hw_info] family=aen sku=E1M-AEN801 hw_rev=r2
[hw_info] serial=A20260514-0001 mfg_date=2026-05-14
```

`alp_hw_info_read` verifies the magic + CRC; an unprogrammed
EEPROM returns `ALP_ERR_NOT_READY` and the application can
react (factory-test fallback, refuse to boot in production,
etc.).

## 5. Assert against the firmware build's expected SKU

For production firmware that's only valid for one SKU, add a
boot-time assertion:

```c
if (alp_hw_info_assert_matches_build(&info,
                                      "E1M-AEN801", "r2") != ALP_OK) {
    /* This firmware was built for a different SoM; refuse to run. */
    k_panic();
}
```

For the common "this firmware was built for exactly this SKU"
check, pass the build's compile-time `ALP_HW_BUILD_SOM_SKU` +
`ALP_HW_BUILD_SOM_HW_REV` (emitted by `scripts/alp_project.py`
from `board.yaml`) as the expected values:

```c
alp_hw_info_assert_matches_build(&info,
                                 ALP_HW_BUILD_SOM_SKU,
                                 ALP_HW_BUILD_SOM_HW_REV);
```

It compares them against the runtime manifest.  Saves
debug time for "why isn't this image booting" -- it's usually
SKU mismatch when the build target was changed but the binary
wasn't reflashed.

## 6. BOARD_ID ADC: the carrier-board companion

The on-module EEPROM manifest is the **sole authoritative source of
the SoM hardware revision** -- there is no SoM-side ADC cross-check of
it (see `<alp/hw_info.h>`).  The BOARD_ID resistor divider below is a
separate, **carrier-board** signal: it identifies the *carrier* (and
its rev), decoded against the board preset's `hw_revisions` table,
independent of the SoM revision.  Wiring it into the runtime read is a
documented future addition -- it is not yet part of `alp_hw_info_read`.

| `hw_rev` | Divider | Expected `mV` (±100 mV bin) |
|----------|---------|----------------------------|
| r1       | 10 kΩ / 10 kΩ | 900 |
| r2       | 4.7 kΩ / 10 kΩ | 1240 (TBD) |
| r3       | 22 kΩ / 10 kΩ | 562 (TBD) |

`alp_hw_info_read` itself returns `ALP_ERR_IO` only when the manifest
is **corrupt** (magic present but a bad `schema_version` / CRC), or
when a caller-supplied expected field disagrees -- not on any
ADC/divider reading:

```c
alp_hw_info_t info;
if (alp_hw_info_read(&info) == ALP_ERR_IO) {
    /* Magic is present but the schema_version or CRC is bad -- the
     * EEPROM manifest is corrupt or was mis-programmed. */
    k_panic();
}
```

See [`docs/board-id.md`](../board-id.md) for the full
rationale + the per-family divider tables.

## 7. Production-floor flow

A typical assembly line runs:

1. Module powered up via the I²C-only programming jig.
2. Visual inspect for solder defects.
3. Run `program_eeprom.py` with the per-device serial.
4. Power-cycle the module + boot the factory-test firmware.
5. Factory-test firmware reads the manifest + verifies the
   board-ID ADC + runs a peripheral self-test (I²C scan,
   ADC noise floor, etc.).
6. On pass, the unit moves to packaging.  On fail, the unit
   gets a defect tag with the failing assertion + recycles.

The programmer's stdout + the factory-test firmware's UART
log together form the **per-device manufacturing record**.
Archive them in a database keyed by serial; warranty claims
get answered by looking up the record.

## See also

- [`include/alp/hw_info.h`](../../include/alp/hw_info.h) -- the
  C surface.
- [`scripts/program_eeprom.py`](../../scripts/program_eeprom.py)
  -- the programmer tool source.
- [`docs/board-id.md`](../board-id.md) -- the BOARD_ID ADC
  companion design.
- [`tests/scripts/test_program_eeprom.py`](../../tests/scripts/test_program_eeprom.py)
  -- unit tests for the layout encoder.
