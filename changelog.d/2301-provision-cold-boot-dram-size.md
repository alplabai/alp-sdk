### Fixed

- `scripts/provision_som.py` (V2N family): `cold_boot_test` now records `dram_size_mib` and `uboot_dram_banner` from the U-Boot `DRAM:` banner it already parses. Before, only `boot_sd_linux` wrote them, so a unit provisioned in two phases (phase B on an eMMC boot) kept failing the ship check with `missing dram_size_mib` even after 3/3 clean cold boots. Found on E1M-V2M103 2026W38-0001 (#2301).
