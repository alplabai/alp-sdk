### Fixed — SoM facts in an example's generated `alp.conf` now follow the board being built (#2597)

An example built for several SoM SKUs took its derived facts from the one
static `som.sku` in `board.yaml`, so `alp-console` built for E1M-AEN803 got
`CONFIG_ALP_SDK_SOM_DRAM_MBIT` / `CONFIG_ALP_SDK_SOM_FLASH_MBIT` unset (both 0)
and its banner dropped the `EXT:` line. `load_board_yaml()` takes an optional
`sku=` override, and `scripts/gen_example_alp_conf.py` additionally writes
`generated/<sku>/alp.conf` (e.g. `generated/aen803/`) for every same-silicon
twin SKU whose fragment differs, only for examples whose `CMakeLists.txt`
carries the selector hook. `alp-console`, `blink`, `pwm-led-fade`,
`drone-autopilot` and `camera-mjpeg-stream` select it from `BOARD` in their
`CMakeLists.txt`. An AEN803 build now yields `SOM_DRAM_MBIT=512` and
`SOM_FLASH_MBIT=256`; AEN801 stays unset. Other examples with an AEN twin
need the same 5-line `CMakeLists.txt` snippet.
