# v2n-emmc-block-stat

Query the V2N module's on-module eMMC via Zephyr's disk-access
(block-storage) subsystem.  Reports sector count + sector size,
reads the first 16 blocks (8 KiB) of the device, looks for the MBR
signature, and asks the driver for any erase-block / wear-level
data it caches.

The example only **reads** -- it never issues a block write to
the eMMC.

## What it shows

1. `disk_access_init("SD")` mounts the eMMC + reads its
   CSD (Card-Specific Data).  `"SD"` is the default Zephyr name
   for the SD0 controller; boards can override with
   `-DEMMC_DISK_NAME=...`.
2. `disk_access_ioctl(GET_SECTOR_SIZE)` +
   `disk_access_ioctl(GET_SECTOR_COUNT)` give the geometry.
   Capacity = product of the two.
3. `disk_access_read(buf, 0, 16)` reads sectors 0..15 -- typically
   the MBR / GPT region.  Standard MBR has `0x55 0xAA` at offset
   510 of block 0; the example reports what it sees.
4. `disk_access_ioctl(GET_ERASE_BLOCK_SZ)` -- one of the few
   wear-relevant fields Zephyr's eMMC driver caches from the
   EXT_CSD register.  Reports "n/a" when the driver returns
   `-ENOTSUP`.

## Expected output (partitioned 16 GiB card)

```
[emmc] sector_size=512  sector_count=30531584  capacity=14908 MiB
[emmc] MBR signature: 55AA (valid)
[emmc] block 0 first 64 bytes:
[emmc] FA31C08ED88EC0...
[emmc] erase block size: 524288 bytes
[emmc] done
```

## Why no writes?

Block-level writes to a production eMMC are destructive in a way
that's hard to recover from (no "undo" without re-flashing the
factory image).  The example deliberately stays read-only so it
can run on any operator's board without consent paperwork; for
write benchmarking + endurance testing the maintainer's HiL rig
has a dedicated test image.

## See also

* Zephyr documentation: `<zephyr/storage/disk_access.h>`.
* `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` -- SD0
  controller pad assignment.
