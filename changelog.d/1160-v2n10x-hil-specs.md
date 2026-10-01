### Added

- HIL: 12 read-only E1M-V2N101 `ssh-run` specs in `tests/hil/v2n101-x-evk/` (ethernet-link, emmc-block, xspi-mtd, optiga-present, gd32-getversion-i2c, gd32-gpiochip, thermal-zones, rtc-ticks, wifi-present, bt-hci-up, systemd-no-failed-units, eeprom-read), derived from the v2m103 specs and reusable on V2N102/V2N103 by targeting `tests/hil/v2n101-x-evk/` explicitly. Unverified on a V2N unit; refs #1160.
