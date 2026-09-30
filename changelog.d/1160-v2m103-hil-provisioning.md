### Added — E1M-V2M103 HIL provisioning specs: EEPROM manifest, eMMC boot config, MAC, chosen SKU (#1160)

Four read-only ssh-run specs under `tests/hil/v2m103-x-evk/` check what
provisioning guarantees on a shipped unit: the EEPROM manifest magic `HPLA`
and SKU `E1M-V2M103` (`v2m103-eeprom-manifest.yaml`), eMMC EXT_CSD[179]=0x08
and [177]=0x02 (`v2m103-emmc-boot-config.yaml`), a locally administered `A2:`
`end0` MAC (`v2m103-mac-provisioned.yaml`), and `/chosen/alp,sku`
(`v2m103-chosen-sku.yaml`). GET_VERSION at BRD_I2C `0x70` was already covered
by `v2m103-gd32-getversion-i2c.yaml`. Not yet bench-run.
