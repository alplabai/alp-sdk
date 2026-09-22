### Added — IMX219/OV5647/IMX296 shields join the CSI-2 first-light example (#2213)

`examples/aen/aen-camera-firstlight` (the portable `<alp/camera.h>`
open/start/capture/release/stop/close first-light bench app for the
E1M-EVK's J5 connector — OV9281 support and the CSI-2 -> CPI pipe itself
landed separately, see #2247) now also builds against the upstream
`raspberry_pi_camera_module_2` (IMX219) shield and this branch's own
`raspberry_pi_camera_module_1` (OV5647) and `raspberry_pi_global_shutter_camera`
(IMX296) shields, one `testcase.yaml` scenario and one `src/main.c` `#elif`
per shield, selecting RAW10 640x480 (IMX219/OV5647) or RAW10 1456x1088
(IMX296) at compile time from the sensor Kconfig the stacked shield
auto-enables. IMX296's single full-frame mode does not fit two buffers in
SRAM0, so the example's `Kconfig` drops
`ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT` / `VIDEO_BUFFER_POOL_NUM_MAX` to 1
and grows `VIDEO_BUFFER_POOL_HEAP_SIZE` to 3.5 MiB for that shield only.

The IMX219, OV5647 and IMX296 paths compile and link against the real
board target but have not yet been run on real silicon — only OV9281 is
bench-verified (2026-09-21, an E1M-AEN803 on the E1M-EVK).
`tests/zephyr/video_sensors` builds OV5647 and IMX296 as compile-coverage
(no emulator) alongside the OV9281 runtime ztest.
