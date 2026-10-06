### Added

- HIL: four E1M-V2M103 `ssh-run` checks encode values observed on the bench, in `tests/hil/v2m103-x-evk/`: `v2m103-pmic-i2c` (ACT88760 at `0x25`, DA9292 at `0x1E`, both on BRD_I2C), `v2m103-ethernet-phy-id` (both PHYs MII ID `001c.c916`), `v2m103-kernel-version` (`6.1.141-cip43`), and the RV3028 at bus 8 address `0x52` added to `v2m103-rtc-ticks`. The V2M101 and V2M102 runners now say to target `../v2m103-x-evk/` explicitly, and that the set is unverified on those SKUs; refs #1160.
