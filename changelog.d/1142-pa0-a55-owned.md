### Changed

- `metadata/e1m_modules/v2n/core-ownership.yaml` records PA0 (`eMMC_V_SEL`) as A55-owned (maintainer decision, #1142): U-Boot sets it before the first MMC access and the Linux `emmc_v_sel_1v8` gpio-hog keeps the eMMC at 1.8 V I/O for HS200, so no CM33 board file may claim it. `metadata/pinmux/v2n.yaml` now carries the core attribution.
