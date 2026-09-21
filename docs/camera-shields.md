# Raspberry-Pi-style CSI-2 camera shields

## Supported camera modules

| Module | Sensor | Shield | Interface / lanes | Modes | Status |
|---|---|---|---|---|---|
| InnoMaker CAM-OV9281 | OV9281 (1 Mpx global-shutter mono) | `innomaker_cam_ov9281` | MIPI CSI-2 D-PHY, 2 lanes | 640x400 GREY8 @100 fps; 1280x720 GREY8 @50 fps; 1280x800 GREY8 @~100 fps | 640x400 configured and **bench-verified** on E1M-AEN803 (E1M-EVK J5), 2026-09-21. 1280x720 and 1280x800 are bench-pending until the next bench pass. |
| RPi Camera Module 1 | OV5647 (5 Mpx raw Bayer) | `raspberry_pi_camera_module_1` | MIPI CSI-2 D-PHY, 2 lanes | up to 2592x1944 SBGGR8/SBGGR10P | Build-only / not run on hardware. |
| RPi Camera Module 2 | IMX219 | `raspberry_pi_camera_module_2` (upstream) | MIPI CSI-2 D-PHY, 2 lanes | 640x480 RAW10 (this repo's first-light example) | Build-only / not run on hardware. |
| RPi Global Shutter Camera | IMX296LQR-C (1.58 Mpx colour global-shutter) | `raspberry_pi_global_shutter_camera` | MIPI CSI-2 D-PHY, 1 lane | 1456x1088 SRGGB10P (all-pixel scan) | Build-only / not run on hardware. |

The E1M-AEN SoM's current camera-connector revision needs a P/N-crossing
adapter regardless of which module is used: on the differential MIPI pairs,
the second pin of each pair is swapped with the third pin of the next pair
in sequence (camera-connector pins 2↔3, 5↔6, 8↔9); every other pin —
grounds, power, control — stays straight. See the OV9281 driver section
below and [`docs/boards/e1m-evk.md`](boards/e1m-evk.md)'s Camera section for
the full adapter note.

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

ADR 0017 Tier-1.5 (third-party permissive port) -- a **port** of the
Apache-2.0 Espressif `esp-video-components` `esp_cam_sensor/sensors/ov9281`
driver onto the upstream Zephyr v4.4 video API, keeping Espressif's register
addresses/values/init tables verbatim. **BENCH-VERIFIED 2026-09-21** on
e1m-aen-evk-02: the 640x400 mode streams real RAW8/GREY8 frames over the
`innomaker_cam_ov9281` shield on J5. See the file header for the source
commit and retirement note, and the "Amendment (2026-09-18)" section of
[ADR 0017](adr/0017-alp-sdk-over-the-vendor-sdk.md) for why this is Tier-1.5
and not Tier-2 (Tier-2 names the opt-in Alif vendor-SDK fork specifically,
which this driver never touches).

**Requires a P/N-crossing adapter on this SoM/EVK combination.** The
E1M-AEN SoM's camera connector wiring currently swaps the P and N wires of
all three MIPI CSI-2 differential pairs (clock lane and both data lanes)
relative to the EVK, so a camera plugged straight into J5 (or J4, the
mux's other input) never synchronizes: the D-PHY leaves Stop-state and the
sensor still answers its I2C probe, but no frame ever arrives. A short
adapter that crosses camera-connector pins 2↔3, 5↔6 and 8↔9 (every other
pin, including the four grounds and the power/control pins, stays
straight) fixes it -- match lane lengths given the 800 Mbit/s/lane rate.
This is what the OV9281 bench pass above used; expect the same fix to be
needed for any other MIPI camera on this SoM revision.

Modes: three, all `GREY` (RAW8 mono), MIPI CSI-2 D-PHY, 2 data lanes,
24 MHz XVCLK. Two are Espressif's: 1280x720 @ 50 fps and 640x400 @ 100 fps
(the latter bench-verified above). The third, 1280x800 @ ~100 fps (the
sensor's full array), is Alp-authored -- derived from the 1280x720 table,
BENCH-PENDING; see the file header and the derivation comment on
`ov9281_mode_1280x800_100fps_regs` for exactly what changed and why. No
RAW10 mode is invented. Controls: exposure, analogue gain (112-step
discrete LUT), test pattern, pixel rate, link frequency.

This is the *streaming* OV9281 driver. A separate portable chip-ID stub,
`chips/ov9281/ov9281.c` (`metadata/chips/ov9281.yaml`, `driver_status: stub`,
advertises up to 1280x800), exists only for the SDK's chip-manifest/backend
scaffolding and does not stream frames -- see that manifest's `notes:` field.

## Driver: IMX296 (`zephyr/drivers/video/imx296.c`)

ADR-0017-ADJACENT -- authored fresh from the Sony IMX296 datasheet (register
map + timing sections), because no permissive-licence IMX296 driver exists
anywhere to consume: not in upstream Zephyr v4.4.1, not in hal_alif, not in
Espressif esp_cam_sensor. Linux's driver and libcamera's IMX296 helpers are
GPL/LGPL and were not read while writing this file. BENCH-UNVERIFIED (no
silicon bench pass on this batch). See the file header for the full
provenance note and retirement path.

Mode: All-pixel scan only -- 1456x1088, the whole effective array as the
sensor transmits it. The datasheet's "Drive Timing Chart for Serial Output in
All-pixel Scan Mode" sends every RAW10 line as 8 + 1440 + 8 = 1456 pixels and
4 + 1080 + 4 = 1088 RAW10 lines per frame: the colour-processing margin is
transmitted, not cropped. 1440x1080 ("recommended recording pixels") is an
image-quality crop inside that frame, left to the consumer; a hardware crop
in the CPI is a possible follow-up. The frame's embedded-data, NULL and
vertical-OB lines use other CSI-2 data types, which the CSI-2 host's IPI does
not pass on. Pixel format `SRGGB10P` (RAW10 packed, RGGB Bayer order -- taken
from the same timing chart, which draws the colour-filter phase at the first
transmitted pixel, not from the physical-array corner diagram elsewhere in
the datasheet, which is a different row: readout starts at the OB side, not
the N1-pin side; the margins are even, so the 1440x1080 crop keeps the same
phase) for the IMX296LQR-C colour part, MIPI CSI-2 D-PHY, **1 data
lane** (unlike OV5647 /
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

**Memory**: RAW10 unpacked is 2 bytes/pixel, so a 1456x1088 frame is
1456 x 1088 x 2 = 3,168,256 bytes (~3.17 MB) -- too large for TCM on the
boards this shield targets; frame buffers must live in SRAM/DDR, not TCM.

## Build coverage

`tests/zephyr/video_sensors` build-only twister-tests all three drivers
together on `native_sim/native/64` against an emulated I2C bus, mirroring
upstream's `tests/drivers/build_all/video`.

## First-light example

`examples/aen/aen-camera-firstlight` opens each shield through the portable
`<alp/camera.h>` API (the upstream `raspberry_pi_camera_module_2`/IMX219
shield also builds against it) and captures one frame with a timeout, on the
E1M-EVK's `e1m_evk_rpi_csi` carrier connector shield. See that example's
README for what each printed line means and the expected result per module
-- the OV9281 path is now bench-verified (2026-09-21, e1m-aen-evk-02); the
IMX219, OV5647 and IMX296 paths compile and link against the real board
target but have not yet been run on real silicon.
