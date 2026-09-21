# Raspberry-Pi-style CSI-2 camera shields

## Supported camera modules

| Module | Sensor | Shield | Interface / lanes | Modes | Status |
|---|---|---|---|---|---|
| InnoMaker CAM-OV9281 | OV9281 (1 Mpx global-shutter mono) | `innomaker_cam_ov9281` | MIPI CSI-2 D-PHY, 2 lanes | 640x400 GREY8 @100 fps; 1280x720 GREY8 @50 fps; 1280x800 GREY8 @~100 fps | **Bench-verified** on an E1M-AEN803 on the E1M-EVK (J5), 2026-09-21: all three modes stream live frames, each at its configured rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps); the sensor test pattern is verified in all three modes. |

E1M-AEN hw_rev r2 (2626-R2)'s camera-connector revision needs a P/N-crossing
adapter regardless of which module is used: each differential pair's N and P
pins are swapped -- 2↔3 (D0), 5↔6 (D1), 8↔9 (CLK); everything else —
grounds, power, control — stays straight. This crossing is for J5's 15-pin
RPi connector only. See the OV9281 driver section below and
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md)'s Camera section for the full
adapter note.

A board-agnostic Zephyr shield under `zephyr/boards/shields/` carries the
Raspberry-Pi-style 15-pin MIPI CSI-2 camera module. It knows nothing about
any specific carrier or SoM -- it just wires the sensor's `fixed-clock` +
endpoint onto the upstream RPi camera shield label contract (`chosen
zephyr,camera`, `&csi_interface`, `&csi_ep_in`, `&csi_i2c`; see upstream
`boards/shields/raspberry_pi_camera_module_2`). A separate carrier
connector shield (owned outside this set, e.g. the E1M-EVK's
`e1m_evk_rpi_csi`) supplies those labels.

| Shield | Sensor | Module oscillator | CCI address | Data lanes |
|---|---|---|---|---|
| `innomaker_cam_ov9281` | OV9281 (1 Mpx global-shutter mono) | 24 MHz | 0x60 | 2 |

## Build command form

```sh
west build -b <board> -- -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"
```

(carrier connector shield first, camera shield second).

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

## Build coverage

`tests/zephyr/video_sensors` runs a real ztest for OV9281 (`ov9281_test.c`)
against an I2C emulator on `native_sim` and `native_sim/native/64`, checking
`get_caps`/`set_format`/register programming/exposure range check.

## First-light example

`examples/aen/aen-camera-firstlight` opens the shield through the portable
`<alp/camera.h>` API and captures one frame with a timeout, on the
E1M-EVK's `e1m_evk_rpi_csi` carrier connector shield. See that example's
README for what each printed line means and the expected result -- **bench-
verified** (2026-09-21, an E1M-AEN803 on the E1M-EVK).
