# v2n-xspi-flash-readwrite

Erase one sector on the V2N module's on-module xSPI NOR, write a
known pattern, read it back, and report the comparison.  Exercises
Zephyr's standard `flash` driver class against the carrier-facing
xSPI bus.

## What it shows

1. Resolving the flash device via `DEVICE_DT_GET(DT_ALIAS(xspi_flash))`.
   The V2N board file (alplabai/alp-zephyr-modules) sets the alias
   to whichever NOR is populated.
2. `flash_erase(offset, 4096)` -- the smallest eraseable unit on
   virtually every consumer-grade NOR.
3. Post-erase `flash_read` confirming the entire sector reads
   0xFF.  An erase that returns success but leaves stale bytes is
   a silent failure mode the example deliberately catches.
4. `flash_write(offset, pattern, 256)` writing a 0x00..0xFF ramp.
5. `flash_read` + `memcmp` verifying the readback.

## Safety

The example writes to **offset 0x1FFF000** (last 4 KiB of a 32 MiB
part) by default to minimise the risk of clobbering a partition the
operator cares about.  Override with `-DXSPI_TEST_OFFSET=0x...` if
you need to test a different sector.  Don't run on a board whose
xSPI NOR holds production firmware.

## Expected output

```
[xspi] using device: mx25l25645g@0
[xspi] post-erase read: all 0xFF (clean)
[xspi] readback OK (memcmp = 0)
[xspi] first 16 read: 000102030405060708090A0B0C0D0E0F
[xspi] done
```

## See also

* Zephyr documentation: `<zephyr/drivers/flash.h>`.
* `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` -- xSPI
  bus pad assignment on the V2N carrier.
