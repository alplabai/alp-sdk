### Added

- Seven read-only E1M-V2M103 `ssh_command` HIL specs under `tests/hil/v2m103-x-evk/` cover the peripherals #1160 listed as uncovered: Ethernet link + gateway ping, eMMC block device + HS200 timing + 64 MiB read, xSPI mtd0/mtd1 + stable mtd0 hash, EEPROM presence at 0x50 on i2c-0 (read-only; SoM and carrier 24c128 collide on that wire and address), OPTIGA ACK at 0x30, GD32 GET_VERSION over i2c-8, and thermal-zone range. Not yet run on the bench (#1160).
