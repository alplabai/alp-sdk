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
their own `SYS_INIT` for it.

New example `examples/aen/aen-camera-firstlight` opens each of the four
RPi-style CSI-2 camera shields the E1M-EVK's J5 connector supports (IMX219,
OV5647, OV9281, IMX296) through `<alp/camera.h>` only, starts the stream, and
waits for one frame with a 2 s timeout — printing CRC32 + a top-bits histogram +
sample row bytes on success, or a diagnosed failure (e.g. `ALP_ERR_NOT_READY`
meaning the sensor never answered its I2C chip-ID probe) otherwise. Frame
buffers move into the global SRAM0 bank (`CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION`)
rather than the HE core's 256 KiB DTCM, which the default 2 MiB pool does not
fit and the CPI's AXI capture master cannot reach regardless of size. The
whole CSI-2 -> CPI pipe has never run on real silicon; all four shield
scenarios are `build_only` in twister.

Retires `examples/aen/aen-camera-regcheck`: its overlay wired the sensor on
CSI port@1 (D-PHY id 1, the DSI PHY) with an `arx3a0` sensor on `i2c2`, both
wrong for the EVK's actual J5 wiring (D-PHY id 0, sensor bus `i2c1`) —
`aen-camera-firstlight` supersedes it with the correct wiring and a real
sensor stack instead of a bind-only placeholder.
