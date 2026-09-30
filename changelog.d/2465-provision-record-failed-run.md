### Fixed

- `scripts/provision_som.py` (V2N family): the `record` step no longer writes bundle facts (`bl2_sha256`, `rootfs_wic_sha256`, `rootfs_bundle_version`, `fip_*`) into the ledger unless `write_xspi`, `write_emmc_boot` and `write_rootfs` all succeeded or were verified already satisfied, and `status --require-shippable` exits 1 (and does not print SHIPPABLE) while any step is failed in the state file. Before, a run that failed before flashing left the unflashed bundle's facts in the ledger and the unit still read SHIPPABLE (#2465).
