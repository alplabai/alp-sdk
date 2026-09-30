### Added — aen-npu-inference-alp-u55 builds for the M55-HP's own Ethos-U55 (#916)

`examples/aen/aen-npu-inference-alp-u55` built only for the M55-HE-paired
Ethos-U55 (128-MAC). It now also builds for the M55-HP core on
`alp_e1m_aen801_m55_hp` / `alp_e1m_aen803_m55_hp` (`ae822fa0e5597ls0/rtss_hp`),
driving that core's own local Ethos-U55 -- confirmed 256-MAC from the SoC
peripherals dtsi (the HE-paired U55 at the same shared DT node is 128-MAC).

Changes:
- New `boards/alp_e1m_aen{801,803}_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay`
  enable the HP-core-local `ethosu55` node (disable `ethosu85`) and set the
  HP TCM `global_base` values (itcm `0x50000000`, dtcm `0x50800000`) for
  `hal_alif`'s `local_to_global()`; model + arena stay in the same
  NPU-reachable SRAM0 region as the HE variant.
- New per-board `boards/alp_e1m_aen{801,803}_m55_{he,hp}_..._rtss_{he,hp}.conf`
  fragments carry the MAC-count Kconfig choice
  (`CONFIG_ETHOS_U55_128`/`_256`), moved out of the shared `prj.conf` since
  it is now per-core.
- `CMakeLists.txt` picks `--accel ethos-u55-128`/`-256` and the matching
  Alif `--system-config` (`RTSS_HE_SRAM_Only`/`RTSS_HP_SRAM_Only`) off
  `BOARD` so the same app source Vela-compiles correctly for either core.
- New `testcase.yaml` (`build_only: true`) covering all four board targets --
  this app needs Alif's proprietary `ensemble_vela.ini` to match the bench
  runtime and boots via MRAM slot0/Flow D, so it is a bench build, not a
  twister run.

Verified: `west build` for `alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`
links (FLASH 419,360 B of 2688 KiB slot0). The Flow C ITCM bench fragments
do NOT fit this model on HP ITCM either (overflows by ~154 KiB, same as the
existing HE build) -- both cores still boot this app via MRAM slot0/Flow D,
not a J-Link ITCM RAM-run.
