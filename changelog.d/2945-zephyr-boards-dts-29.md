### Fixed — aen: board layer parity across AEN401/601/801/803 (MPU table, flex-IO, I2C0, power GPIO ports, TMP112) (#2945, #2946, #2947, #2948, #2984)

The AEN401/AEN601 board trees had drifted from the shared E1M-AEN SoM facts
the AEN801/803 trees carry:

- The MRAM MPU table (mpu_regions_e8.c) was gated on CONFIG_SOC_SERIES_E8, so
  AEN401/601 fell back to the generic FLASH_0 map with no MRAM_DEVICE write
  window and no read-only ATOC span. Gate it on
  CONFIG_CPU_HAS_CUSTOM_FIXED_SOC_MPU_REGIONS only and set that from a
  hand-maintained Kconfig in both board directories.
- ALIF_FLEX_IO_1V8 depended on SOC_SERIES_E8 and defaulted on only for E8
  boards, leaving the +1V8 VDD_IO_FLEX flex pads (RV-3028 /INT, E_WIFI_NRST)
  in 3.3 V mode on AEN401/601. Widen the dependency to the Ensemble family and
  default it on for both boards.
- AEN401 lacked the CONFIG_TMP112 default-n guard (duplicate tmp112_init()).
- AEN401 never enabled SoC I2C2 / alp-i2c0 (E1M I2C0, EEPROM manifest bus):
  emit the generator's e1m_i2c0 block in its on-module dtsi, add pinctrl_i2c2,
  and drop the now-redundant copy from the sdcard-file overlay.
- som_pd_backlight (gpio5) and som_pd_eth_phy (gpio11) referenced GPIO ports
  the SoC dtsi leaves disabled, so SoM quiesce failed with ALP_ERR_NOT_READY on
  a stock board. The generator now enables every non-LPGPIO port a present
  power domain uses; AEN801/803 HE/HP and the AEN401 dtsi are regenerated.

Tests: the AEN401 on-module dtsi parity test follows the new block, and a new
class checks the shared defaults on every AEN board directory.
