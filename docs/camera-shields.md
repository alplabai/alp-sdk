# Raspberry-Pi-style CSI-2 camera shields

Three board-agnostic Zephyr shields under `zephyr/boards/shields/` carry
Raspberry-Pi-style 15-pin MIPI CSI-2 camera modules. None of the shields
knows about any specific carrier or SoM — each just wires its sensor's
`fixed-clock` + endpoint onto the upstream RPi camera shield label contract
(`chosen zephyr,camera`, `&csi_interface`, `&csi_ep_in`, `&csi_i2c`; see
upstream `boards/shields/raspberry_pi_camera_module_2`). A separate carrier
connector shield (owned outside this set, e.g. the E1M-EVK's
`e1m_evk_rpi_csi`) supplies those labels.

| Shield | Sensor | Module oscillator | CCI address | Data lanes |
|---|---|---|---|---|
| `raspberry_pi_camera_module_1` | OV5647 (5 Mpx raw Bayer) | 25 MHz | 0x36 | 2 |
| `innomaker_cam_ov9281` | OV9281 (1 Mpx global-shutter mono) | 24 MHz | 0x60 | 2 |
| `raspberry_pi_global_shutter_camera` | IMX296LQR-C (1.58 Mpx colour global-shutter) | 54 MHz | 0x1A | 1 |

## Build command form

```sh
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera"
```

(carrier connector shield first, camera shield second).

## Driver: OV5647 (`zephyr/drivers/video/ov5647.c`)

ADR 0017 Tier-1 -- an **upstream-PENDING backport** of open upstream PR
zephyrproject-rtos/zephyr#119301 @ `d81b1f630ee0f5078de6e3a47e7bc1b3613424c2`,
carried verbatim (Apache-2.0) because it is not yet in the pinned Zephyr
v4.4.1 base. Delete the driver, its Kconfig, and its binding the moment the
Zephyr pin advances past a revision containing that PR -- see the file
header for the exact retirement steps and
[ADR 0017](adr/0017-alp-sdk-over-the-vendor-sdk.md).

Modes: SBGGR8 / SBGGR10P raw Bayer, any 4-pixel-aligned resolution up to
2592x1944, MIPI CSI-2 D-PHY, 2 data lanes, 6-27 MHz XVCLK. Controls:
auto/manual gain, auto/manual exposure, H/V flip, test pattern, pixel rate.

**No `pwdn-gpios`**: the upstream binding treats it as optional (not
`required: true`), and the E1M-EVK's 15-pin RPi camera connector carries no
PWDN GPIO line to wire it to — the shield overlay omits the property rather
than faking a GPIO, and the driver compiles the PWDN code path out entirely
when no instance declares it.

## Driver: OV9281 (`zephyr/drivers/video/ov9281.c`)

ADR 0017 Tier-2 -- a **port** of the Apache-2.0 Espressif
`esp-video-components` `esp_cam_sensor/sensors/ov9281` driver onto the
upstream Zephyr v4.4 video API, keeping Espressif's register
addresses/values/init tables verbatim. BENCH-UNVERIFIED (no silicon bench
pass on this batch). See the file header for the source commit and
retirement note.

Modes: only the two Espressif ships -- `GREY` (RAW8 mono) 1280x720 @ 50 fps
and 640x400 @ 100 fps, MIPI CSI-2 D-PHY, 2 data lanes, 24 MHz XVCLK (no
RAW10 or 1280x800 mode is invented). Controls: exposure, analogue gain
(112-step discrete LUT), test pattern, pixel rate, link frequency.

## Driver: IMX296 (`zephyr/drivers/video/imx296.c`)

ADR-0017-ADJACENT -- authored fresh from the Sony IMX296 datasheet (register
map + timing sections), because no permissive-licence IMX296 driver exists
anywhere to consume: not in upstream Zephyr v4.4.1, not in hal_alif, not in
Espressif esp_cam_sensor. Linux's driver and libcamera's IMX296 helpers are
GPL/LGPL and were not read while writing this file. BENCH-UNVERIFIED (no
silicon bench pass on this batch). See the file header for the full
provenance note and retirement path.

Mode: All-pixel scan only -- 1440x1080 (the sensor's own "recording pixel"
output size, not the 1456x1088 effective-silicon count -- the extra rows/
columns are a colour-processing margin the sensor crops internally before
CSI-2 output), `SGBRG10P` (RAW10 packed, GBRG Bayer order for the
IMX296LQR-C colour part), MIPI CSI-2 D-PHY, **1 data lane** (unlike OV5647 /
OV9281 above, both 2-lane parts), 37.125/54/74.25 MHz INCK (any other
frequency is rejected at runtime with `-ENOTSUP`), 60.3 frame/s fixed.
Controls: exposure (in integration-time lines, converted internally to the
inversely-related SHS register), analogue gain (0.1 dB step, 0-48 dB),
H/V flip, pixel rate (118.8 Mpix/s, read-only), link frequency (594 MHz,
read-only).

The datasheet defines no readable chip/product-ID register for this part
(its "Chip ID = 02h".."13h" register-map headings name address banks, not a
device-identification value), so the driver's probe instead does a
documented readable-register sanity check: it reads back the STANDBY
register and checks it holds its documented power-on-reset default.

**Memory**: RAW10 unpacked is 2 bytes/pixel, so a 1440x1080 frame is
1440 x 1080 x 2 ~= 3.11 MB -- too large for TCM on the boards this shield
targets; frame buffers must live in SRAM/DDR, not TCM.

## Build coverage

`tests/zephyr/video_sensors` build-only twister-tests all three drivers
together on `native_sim/native/64` against an emulated I2C bus, mirroring
upstream's `tests/drivers/build_all/video`.
