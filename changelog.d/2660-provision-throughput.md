### Changed — provisioning tool: fewer wedges and reboots, truthful labels (#2660)

`provision_som.py` gains `--linux-host HOST` (pins the target for one run) and, with no pinned
host, an `--only` run that needs Linux now does the console login and IP discovery itself.
The SCPI client spaces commands by `power.min_gap_s` (default 0.3 s). `dsw1_emmc_insert_sd`
reports the boot mode BL2 observed and no longer tells the operator to move DSW1. The
`gd32_flash` probe is satisfied by the bridge `GET_VERSION` plus the ledger's
`gd32_fw_version`, without the SWD readback that can wedge i2c-8; `--force-step gd32_flash`
still verifies over SWD. `functional_test` already reuses the live boot (no extra power cycle).
