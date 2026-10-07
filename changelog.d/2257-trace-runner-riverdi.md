### Added — aen-trace-runner runs on the Riverdi RVT121 12.1" LVDS panel (#2257)

`-DTR_PANEL=rvt121` builds the game for the 1280x800 landscape panel (shield `e1m_evk_rvt121hvdfwca0`) with `TR_RENDER=M55`, IMU steering and `TR_PANEL_HZ=30`. The default `TR_PANEL=rk055` build is unchanged. CMake refuses `TR_RENDER=A32`, `TR_CAMERA` and `TR_INPUT_NPU` with the new panel until phase 2. Build-only, not run on hardware.
