# aen-isp-capture — real sensor frame through the Alif ISP-Pico

Bench proof for the Alif ISP-Pico (VeriSilicon ISP Nano) pipeline on the
E1M-AEN801/AEN803 (Ensemble E8, M55-HE): a real sensor frame through
`sensor -> csi -> cam -> isp -> memory`, with AE (auto exposure/gain) and
AWB (auto white balance) running through `isp_pico.c`'s standard Zephyr
video ctrl registry — not a fixed manual exposure, the whole point of
driving a real ISP rather than a raw sensor capture (see
`examples/aen/aen-camera-firstlight` for that raw-capture proof instead).
Mirrors Alif's own `sdk-alif` `samples/drivers/viewfinder` recipe. See
`docs/camera-shields.md` for the full driver/shield/control reference and
`changelog.d/2287.md` for the IMX296 bring-up's bench history.

## OV5647 (default) vs. IMX296 (`-DAEN_ISP_IMX296=ON`)

| | OV5647 (default) | IMX296 (`-DAEN_ISP_IMX296=ON`) |
|---|---|---|
| Shield | `raspberry_pi_camera_module_1` | `raspberry_pi_global_shutter_camera` |
| ISP input | `SBGGR10P`, 640x480 | `SRGGB10P`, 1280x960 ROI crop |
| Frames captured | 30 | 1 (AE off) or 60 (AE on) — see `AEN_ISP_N_FRAMES` |
| AWB/CCM calibration | hal_alif patch 0008, fitted to this module | **none — falls through to the stock ARX3A0 defaults; colour is NOT calibrated for IMX296** |
| AE envelope | hal_alif patch 0011 | hal_alif patch 0013 |
| Bench status | bench-verified (see `docs/camera-shields.md`) | data path bench-verified (run 297, ISP-Pico + AE, runs 298-310); colour uncalibrated, image quality not verified |

```bash
# OV5647 (default):
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-isp-capture -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_camera_module_1"

# IMX296, AE off (one frame, manual-control captures possible -- see below):
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-isp-capture -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" \
    "-DAEN_ISP_IMX296=ON" "-DEXTRA_CONF_FILE=overlay-no-ae.conf"

# IMX296, AE on (60 frames, hal_alif patch 0013's envelope):
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-isp-capture -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" \
    "-DAEN_ISP_IMX296=ON"
# flash + run per docs/aen-bench-bringup.md.
```

## Diagnostic knobs (IMX296 only)

These exist to isolate AE/exposure/gain bugs on the bench — see
`CMakeLists.txt`'s own comments for the full history behind each one.

- **`-DAEN_ISP_N_FRAMES=<n>`** — override the frame count (default: 1 with
  AE off, 60 with AE on). Lets an AE-off run capture more than the
  original single first-light frame, e.g. to check a flat readback isn't
  specific to the AE loop.
- **`-DAEN_ISP_IMX296_EXPOSURE_LINES=<n>`** (AE off only, 1..1104) — pin
  `VIDEO_CID_EXPOSURE` to an exact line count instead of letting AE choose
  it, so a specific exposure can be held for a whole capture run
  independent of what AE would have converged to. Values above 1104 are
  rejected by `imx296.c`'s own control range (`-EINVAL`, see that
  driver's own comment on why the ceiling isn't the datasheet's full
  1114).
- **`-DAEN_ISP_IMX296_GAIN=<n>`** (AE off only, 0.1 dB tenths, 0..480) —
  same idea, for `VIDEO_CID_ANALOGUE_GAIN`. Both options are needed
  together to pin exposure and gain independently, since AE normally
  moves them together and any failing capture so far has had both
  confounded.
- **Register dumps** — the AE-on build prints IMX296's own `SHS`
  (0x308D-0x308F) and `GAIN` (0x3204-0x3205) registers directly over I2C
  every few frames (`REG_DUMP_EVERY`), a genuine hardware readback of
  what the writeback actually landed in the sensor, not just what the
  library computed as its target.

## What it proves, and what it doesn't

Proves: the ISP-Pico data path (input-format mapping, debayer/colour
pipeline, DMA to memory) runs correctly for both sensors, and IMX296's
own AE loop converges through the same `isp -> cam -> csi -> sensor`
control chain OV5647 uses. Does **not** prove IMX296 colour is correct —
there is no IMX296-fitted AWB/CCM calibration table yet (patches
0008/0011 are OV5647-only), so an IMX296 build's U/V output reflects the
stock ARX3A0 calibration applied to data it was never fitted to, not a
validated picture.
