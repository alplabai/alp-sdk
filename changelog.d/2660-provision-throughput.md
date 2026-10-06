### Changed — provisioning tool: SCPI pacing, `--linux-host`, truthful dsw1 labels (#2660)

`provision_som.py` gains `--linux-host HOST`, which sets the target host for one run (an EEPROM MAC
change still forces rediscovery). The SCPI client spaces commands by `bench.yaml`
`power.min_gap_s` (default 0.3 s) to keep the SPD3303X LAN socket from wedging.
`dsw1_emmc_insert_sd` reports the boot mode BL2 observed and no longer tells the operator to move DSW1.
