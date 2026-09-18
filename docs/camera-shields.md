# Raspberry-Pi-style CSI-2 camera shields

Two board-agnostic Zephyr shields under `zephyr/boards/shields/` carry
Raspberry-Pi-style 15-pin MIPI CSI-2 camera modules. Neither shield knows
about any specific carrier or SoM — each just wires its sensor's
`fixed-clock` + endpoint onto the upstream RPi camera shield label contract
(`chosen zephyr,camera`, `&csi_interface`, `&csi_ep_in`, `&csi_i2c`; see
upstream `boards/shields/raspberry_pi_camera_module_2`). A separate carrier
connector shield (owned outside this pair, e.g. the E1M-EVK's
`e1m_evk_rpi_csi`) supplies those labels.

| Shield | Sensor | Module oscillator | CCI address | Data lanes |
|---|---|---|---|---|
| `raspberry_pi_camera_module_1` | OV5647 (5 Mpx raw Bayer) | 25 MHz | 0x36 | 2 |
| `innomaker_cam_ov9281` | OV9281 (1 Mpx global-shutter mono) | 24 MHz | 0x60 | 2 |

## Build command form

```sh
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"
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

## Build coverage

`tests/zephyr/video_sensors` build-only twister-tests both drivers together
on `native_sim/native/64` against an emulated I2C bus, mirroring upstream's
`tests/drivers/build_all/video`.

## First-light example

`examples/aen/aen-camera-firstlight` opens each shield through the portable
`<alp/camera.h>` API (a third shield, `raspberry_pi_camera_module_2`/IMX219,
also builds against it) and captures one frame with a timeout, on the
E1M-EVK's `e1m_evk_rpi_csi` carrier connector shield. See that example's
README for what each printed line means and the expected result per module
-- the whole CSI-2 -> CPI pipe has never run on real silicon.
