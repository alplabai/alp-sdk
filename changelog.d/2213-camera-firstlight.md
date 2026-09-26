### Added — OV5647 shield joins the CSI-2 first-light example (#2213)

`examples/aen/aen-camera-firstlight` (the portable `<alp/camera.h>`
open/start/capture/release/stop/close first-light bench app for the
E1M-EVK's J5 connector — OV9281 support and the CSI-2 -> CPI pipe itself
landed separately, see #2247) now also builds against this branch's own
`raspberry_pi_camera_module_1` (OV5647) shield, one `testcase.yaml`
scenario and one `src/main.c` `#elif`, selecting RAW10 640x480 at compile
time from `CONFIG_VIDEO_OV5647`, the Kconfig the stacked shield
auto-enables.

OV9281 is bench-verified (2026-09-21, an E1M-AEN803 on the E1M-EVK); the
OV5647 path's bring-up and bench status is tracked in issue #2248.
`tests/zephyr/video_sensors` builds OV5647 as a real runtime ztest
(against its own I2C emulator) alongside the OV9281 one.
