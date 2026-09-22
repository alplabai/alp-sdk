# Raspberry-Pi-style CSI-2 camera shields

## Supported camera modules

| Module | Sensor | Shield | Interface / lanes | Modes | Status |
|---|---|---|---|---|---|
| InnoMaker CAM-OV9281 | OV9281 (1 Mpx global-shutter mono) | `innomaker_cam_ov9281` | MIPI CSI-2 D-PHY, 2 lanes | 640x400 GREY8 @100 fps; 1280x720 GREY8 @50 fps; 1280x800 GREY8 @~100 fps | **Bench-verified** on an E1M-AEN803 on the E1M-EVK (J5), 2026-09-21: all three modes stream live frames, each at its configured rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps); the sensor test pattern is verified in all three modes. |
| RPi Camera Module 1 | OV5647 (5 Mpx raw Bayer) | `raspberry_pi_camera_module_1` | MIPI CSI-2 D-PHY, 2 lanes | up to 2592x1944 SBGGR8/SBGGR10P | **Bench-attempted, BLOCKED**: `alp_camera_open` fails at D-PHY Stop-state (see the driver section below). NOT bench-verified. Needs the J5 pin-11 pull-up rework -- [`docs/boards/e1m-evk.md`](boards/e1m-evk.md). |
| RPi Camera Module 2 | IMX219 | `raspberry_pi_camera_module_2` (upstream) | MIPI CSI-2 D-PHY, 2 lanes | 640x480 RAW10 (this repo's first-light example) | Build-only / not run on hardware. |
| RPi Global Shutter Camera | IMX296LQR-C (1.58 Mpx colour global-shutter) | `raspberry_pi_global_shutter_camera` | MIPI CSI-2 D-PHY, 1 lane | 1456x1088 SRGGB10P (all-pixel scan) | Build-only / not run on hardware. |

E1M-AEN hw_rev r2 (2626-R2)'s camera-connector revision needs a P/N-crossing
adapter regardless of which module is used: each differential pair's N and P
pins are swapped -- 2↔3 (D0), 5↔6 (D1), 8↔9 (CLK); everything else —
grounds, power, control — stays straight. This crossing is for J5's 15-pin
RPi connector only. See the OV9281 driver section below and
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md)'s Camera section for the full
adapter note.

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

**Bench-attempted, BLOCKED at D-PHY Stop-state (2026-09-22, an
E1M-AEN803 serial 2026W36-0001 on an E1M-EVK hw_rev 2626-r2).** This is
NOT bench-verified. The InnoMaker CAM-OV5647 module has no pull-up of
its own on J5 pin 11 and needs the pull-up rework described in
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md) before it answers on I2C
at all. With the module powered and answering I2C, `alp_camera_open`
fails at `ALP_ERR_TIMEOUT` with `E: D-PHY not locked to Stop-state. PHY
status - 0x00010000 DPHY ID: 0`. That `PHY status` field is `CSI_PHY_RX`
(`0x49033048`), the register the driver's log line prints, and it reads
`0x00010000` — `RXULPSCLKNOT` set, `RXCLKACTIVEHS` (bit17) clear.
`CSI_PHY_STOPSTATE` (`0x4903304c`) is a separate register and reaches
only `0x00000001` (bit0 `STOPSTATEDATA_0`), and only after the receiver
is configured; bit1 (`DATA_1`) and bit16 (`CLK`) never assert. For
contrast, the OV9281 path below reaches `CSI_PHY_STOPSTATE` =
`0x00010003` (`CLK|DATA_1|DATA_0`) on the same receiver.

Ruled out by control-validated bench runs: module power, the sensor's
own MIPI PHY being disabled (register `0x3018` = `0x44` decodes to
`PHY_PD_MIPI` 0 / `PHY_PD_LPRX` 0 / `MIPI_EN` 1), the mainline
"coax lanes into LP-11" park sequence applied in the correct running
state, sensor-before-receiver ordering, and the receiver's D-PHY
frequency bin (identical behaviour at `hsfreqrange 0x16` and at `0x09`,
the bin the OV9281 streams in).

**Root cause: a fault in the module under test, on `DATA_1` and `CLK`.**
Put the sensor in mainline's running-parked state (`0x0100` = `0x01`,
then `0x4800` = `0x25`, `0x4202` = `0x0f`, `0x300d` = `0x01`), in which
all three lanes should idle at LP-11, and sample `CSI_PHY_STOPSTATE` 50
times at 20 ms with the receiver configured: `DATA_0` reads 50/50 while
`DATA_1` and `CLK` read 0/50. A marginal LP swing or a module supply
problem would have made `DATA_0` flicker too, so that is excluded. The
wiring is exonerated by interleaving: the same cable, P/N-crossing
adapter and connector gave OV5647 `DATA_0`-only, then an OV9281 reaching
`0x00010003` with a CRC-verified frame, then OV5647 `DATA_0`-only again.

Two driver defects were found along the way and are real regardless of
this module, both tracked in issue #2248 and both belonging upstream in
zephyrproject-rtos/zephyr#119301 rather than in a local patch (see the
retirement note at the top of `zephyr/drivers/video/ov5647.c`):

1. The backport never performs mainline's LP-11 park. Mainline's
   `ov5647_power_on()` calls `ov5647_stream_stop()` under the comment
   "Stream off to coax lanes into LP-11 state"; the vendored driver only
   ever writes `0x0100`. In software standby this part presents no LP-11
   at all (`CSI_PHY_STOPSTATE` = `0x00000000`), so on a receiver that
   gates on Stop-state before stream start — as this one does — even a
   healthy OV5647 would fail to open.
2. The declared pixel rate is off by 2.1x. `PIXEL_RATE = XVCLK * 10 / 3`
   = 83333333 comes from a datasheet fps figure, but the default PLL the
   driver leaves in place (`0x3034` = `0x1a`, `0x3035` = `0x11`,
   `0x3036` = `0x69`, `0x3037` = `0x03`) gives VCO = 875 MHz, i.e. 875
   Mbps per lane and a 175 MHz pixel rate. The receiver therefore picks
   `hsfreqrange 0x16` (450 Mbps) where `0x29` (900) is correct. This
   does not affect Stop-state but will matter once Stop-state passes.

## Driver: OV9281 (`zephyr/drivers/video/ov9281.c`)

ADR 0017 Tier-1.5 (third-party permissive port) -- a **port** of the
Apache-2.0 Espressif `esp-video-components` `esp_cam_sensor/sensors/ov9281`
driver onto the upstream Zephyr v4.4 video API, keeping Espressif's register
addresses/values/init tables verbatim, plus one Alp-derived 1280x800 table
(see the driver's file header). **BENCH-VERIFIED 2026-09-21** on an
E1M-AEN803 on the E1M-EVK: live GREY8 frames captured and CRC-verified via
an SWD dump in all three modes over the `innomaker_cam_ov9281` shield on J5
-- a 60-frame wall-clock burst per mode measured 640x400 ~100 fps, 1280x720
~50 fps and 1280x800 ~100 fps, each at its configured rate, and the sensor
test pattern was verified in all three modes. See the file header for the
source commit and retirement note, and the "Amendment (2026-09-18)" section
of [ADR 0017](adr/0017-alp-sdk-over-the-vendor-sdk.md) for why this is
Tier-1.5 and not Tier-2 (Tier-2 names the opt-in Alif vendor-SDK fork
specifically, which this driver never touches).

**Requires a P/N-crossing adapter on this SoM/EVK combination.** E1M-AEN
hw_rev r2 (2626-R2)'s camera connector wiring currently swaps the P and N
wires of all three MIPI CSI-2 differential pairs (clock lane and both data
lanes) relative to the EVK, so a camera plugged straight into J5 never
synchronizes: the D-PHY leaves Stop-state and the sensor still answers its
I2C probe, but no frame ever arrives. J4, the mux's other 34-pin MIPI B2B
input, shares the same SoM pads and is expected to be affected too, but has
not been bench-tested. A short adapter that crosses J5's camera-connector
pins 2↔3, 5↔6 and 8↔9 (every other pin, including the four grounds and the
power/control pins, stays straight) fixes it -- match lane lengths given
the 800 Mbit/s/lane rate. This is what the OV9281 bench pass above used;
expect the same fix to be needed for any other MIPI camera on this SoM
revision.

Modes: three, all `GREY` (RAW8 mono), MIPI CSI-2 D-PHY, 2 data lanes,
24 MHz XVCLK. Two are Espressif's: 1280x720 @ 50 fps and 640x400 @ 100 fps.
The third, 1280x800 @ ~100 fps (the sensor's full array), is Alp-authored --
derived from the 1280x720 table; see the file header and the derivation
comment on `ov9281_mode_1280x800_100fps_regs` for exactly what changed and
why. All three are bench-verified above, each running at its configured
rate. No RAW10 mode is invented. Controls: exposure, analogue gain
(112-step discrete LUT), test pattern, pixel rate, link frequency.

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

`tests/zephyr/video_sensors` runs a real ztest for OV9281 (`ov9281_test.c`)
against an I2C emulator on `native_sim` and `native_sim/native/64`, checking
`get_caps`/`set_format`/register programming/exposure range check; OV5647
and IMX296 stay compile-coverage in that same runtime suite (no emulator --
see `app.overlay`), mirroring upstream's `tests/drivers/build_all/video`
shape for the compile-only pair.

## First-light example

`examples/aen/aen-camera-firstlight` opens each shield through the portable
`<alp/camera.h>` API (the upstream `raspberry_pi_camera_module_2`/IMX219
shield also builds against it) and captures one frame with a timeout, on the
E1M-EVK's `e1m_evk_rpi_csi` carrier connector shield. See that example's
README for what each printed line means and the expected result per module
-- the OV9281 path is now bench-verified (2026-09-21, an E1M-AEN803 on the
E1M-EVK); the IMX219 and IMX296 paths compile and link against the real
board target but have not yet been run on real silicon. The OV5647 path
has been bench-attempted but is BLOCKED at D-PHY Stop-state (see the
OV5647 driver section above) -- it is not bench-verified.
