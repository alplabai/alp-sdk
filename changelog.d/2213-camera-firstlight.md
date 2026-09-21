### Added — Portable RAW/GREY8 camera formats + a CSI-2 first-light example for the E1M-EVK (#2213)

`<alp/peripheral.h>`'s `alp_pixfmt_t` gains three sensor-native formats,
appended after `ALP_PIXFMT_NV12` (existing values unchanged, ABI-safe):
`ALP_PIXFMT_GREY8` (8-bit mono), `ALP_PIXFMT_RAW8` (sensor-native 8-bit,
Bayer or mono), and `ALP_PIXFMT_RAW10` (sensor-native 10-bit, always
delivered unpacked as one 16-bit little-endian sample per pixel). The
portable Zephyr camera backend (`src/backends/camera/zephyr_video.c`) maps
each to the first entry a sensor's own `video_get_caps()` advertises for
that bit depth (`GREY8` -> `VIDEO_PIX_FMT_GREY`; `RAW8` -> any of
`SBGGR8`/`SGBRG8`/`SGRBG8`/`SRGGB8`/`GREY`; `RAW10` -> any `S*10P`/`Y10P`),
so one `alp_camera_open()` call works unmodified whatever Bayer order the
stacked sensor happens to use. The ISP-backed backends (`alif_isp_pico`,
`v2n_n44_isp`) output processed RGB only, so they reject the three new
formats with `ALP_ERR_NOSUPPORT` instead of silently returning RGB.

The shared D-PHY driver (`zephyr/drivers/mipi_dphy/dphy_dw.c`) now powers the
D-PHY at init: it enables the CGU HFOSC and 100 MHz clocks and clears the VBAT
`PWR_CTRL` D-PHY power masks, isolation and 1.8 V bypass. Without that the
D-PHY never reaches Stop-state and camera opens fail. Apps no longer need
their own `SYS_INIT` for it. `zephyr/kconfigs/vendor-alif-peripherals.kconfig`
also gains the `CONFIG_MIPI_DPHY_LOG_LEVEL` module log-level Kconfig
`dphy_dw.c`'s `LOG_MODULE_REGISTER` names but that no upstream Kconfig class
defined, which failed any `CONFIG_LOG=y` build linking the D-PHY driver.

New example `examples/aen/aen-camera-firstlight` opens each of the four
RPi-style CSI-2 camera shields the E1M-EVK's J5 connector supports (IMX219,
OV5647, OV9281, IMX296) through `<alp/camera.h>` only, starts the stream, and
waits for one frame with a 2 s timeout — printing CRC32 + a top-bits histogram +
sample row bytes on success, or a diagnosed failure (e.g. `ALP_ERR_NOT_READY`
meaning the sensor never answered its I2C chip-ID probe) otherwise. Frame
buffers move into the global SRAM0 bank (`CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION`)
rather than the HE core's 256 KiB DTCM, which the default 2 MiB pool does not
fit and the CPI's AXI capture master cannot reach regardless of size. All
three camera backends now allocate those buffers with
`video_buffer_aligned_alloc()` at `CONFIG_VIDEO_BUFFER_POOL_ALIGN` (64 by
default) instead of `video_buffer_alloc()`'s 4-byte alignment on the M55 —
the Alif CPI rejects a non-8-byte-aligned buffer with `-ENOBUFS` at enqueue,
which the portable backend maps to `ALP_ERR_IO`. The 2 MiB SRAM0 pool also
needs `CONFIG_SYS_HEAP_AUTO=y`, set in the example's `prj.conf`: Zephyr
defaults to `SYS_HEAP_SMALL_ONLY` whenever the kernel's SRAM is <= 256 KB
(the M55-HE's DTCM), and that heap kind cannot span a pool bigger than
262136 bytes — `src/camera_dispatch.c` now fails the build with a pointer to
the fix rather than let it misbehave at run time.

`CONFIG_VIDEO_ALIF_CAM_EXTENDED` now defaults on for `SOC_SERIES_E8`
(`zephyr/kconfigs/vendor-alif-peripherals.kconfig`). `video_alif.c` only
sets `CAM_CFG.AXI_PORT_EN` inside that option; with it off the E8 CPI never
wrote a frame to memory, yet it still raised STOP for every frame, so the
portable camera backend handed back the untouched buffer and
`alp_camera_capture()` reported `ALP_OK`. On e1m-aen-evk-02 (OV9281 on J5)
a pool pre-filled with `0xA5` came back byte-for-byte unchanged, test
pattern on or off, with `CAM_CFG` = `0x00030013` (bit2 `AXI_PORT_EN`
clear); with the fix, `0x00030017`. Alif's own E8 DK board confs set this
option for the same reason.

**The whole CSI-2 -> CPI pipe is now bench-verified for one sensor.**
2026-09-21 on e1m-aen-evk-02: the `innomaker_cam_ov9281` shield streams
real 640x400 GREY8 frames at 800 Mbit/s/lane, once fitted with the
P/N-crossing adapter this SoM revision's camera connector needs — its
MIPI CSI-2 wiring swaps the P and N wires of all three differential pairs
(clock lane and both data lanes) relative to the EVK, so a camera plugged
straight into J5 (or J4, the mux's other input) answers its I2C chip-ID
probe but no frame ever arrives. An adapter crossing camera-connector pins
2↔3, 5↔6 and 8↔9 (every other pin straight) fixes it; see
`docs/boards/e1m-evk.md`'s Camera section and `docs/camera-shields.md`.
The IMX219, OV5647 and IMX296 paths remain BENCH-UNVERIFIED. `ov9281.c`
also gains a third, Alp-authored 1280x800 GREY8 mode (the sensor's full
array, derived from the 1280x720 table) — BENCH-PENDING alongside
1280x720. `tests/zephyr/video_sensors` now runs a real ztest for OV9281
(`ov9281_test.c`) against an I2C emulator on native_sim, checking
`get_caps`/`set_format`/register programming/exposure clamping instead of
only compiling the driver; OV5647 and IMX296 stay `build_only` in
twister — twister has no bench access.

Retires `examples/aen/aen-camera-regcheck`: its overlay wired the sensor on
CSI port@1 (D-PHY id 1, the DSI PHY) with an `arx3a0` sensor on `i2c2`, both
wrong for the EVK's actual J5 wiring (D-PHY id 0, sensor bus `i2c1`) —
`aen-camera-firstlight` supersedes it with the correct wiring and a real
sensor stack instead of a bind-only placeholder.
