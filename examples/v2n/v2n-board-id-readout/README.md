# v2n-board-id-readout

Reads + prints the SoM's hardware-info manifest from the on-module
24C128 EEPROM, then asserts the firmware build is intended for the
SKU on the module.

## What it does

1. Calls `alp_hw_info_read(&info)` -- internally opens
   `ALP_E1M_I2C0`, reads the 128-byte manifest at offset 0, validates
   magic + schema_version + CRC32, populates the struct.
2. Prints every field (family, SKU, hw_rev, serial, mfg date).
3. Asserts `info.som_sku == "E1M-V2N101"` via
   `alp_hw_info_assert_matches_build()`.

## Expected output (factory-programmed module)

```
[board-id] read SoM hardware-info manifest
[board-id] family    = v2n
[board-id] sku       = E1M-V2N101
[board-id] hw_rev    = r1
[board-id] serial    = ALP-V2N101-26W19-00042
[board-id] mfg date  = 2026-05-09
[board-id] firmware build is for this SoM family
[board-id] done
```

## Expected output (unprogrammed module)

```
[board-id] read SoM hardware-info manifest
[board-id] manifest CRC mismatch or magic byte wrong
[board-id]   factory programming has not run on this
[board-id]   module; flag for production-test follow-up.
[board-id] done
```

## See also

* [`docs/board-id.md`](../../../docs/board-id.md) -- full identification
  flow (EEPROM manifest + planned BOARD_ID ADC cross-check).
* [`<alp/hw_info.h>`](../../../include/alp/hw_info.h) -- public API.
* [`scripts/program_eeprom.py`](../../../scripts/program_eeprom.py) --
  production-test programmer.
