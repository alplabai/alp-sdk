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

# IMX335, AE off (issue #2327 Stage B, AE-off variant, BUILD-ONLY -- not bench-run):
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-isp-capture -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi innomaker_cam_imx335" \
    "-DAEN_ISP_IMX335=ON" "-DEXTRA_CONF_FILE=overlay-no-ae.conf"

# IMX335, AE on (hal_alif patch 0014's envelope; bench-verified, run 332 and again 2026-10-07: 60 YUV420 1280x960 frames, `ae_stable=1` from f45, Y mean 146):
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-isp-capture -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi innomaker_cam_imx335" \
    "-DAEN_ISP_IMX335=ON"
```

## IMX335 (`-DAEN_ISP_IMX335=ON`, issue #2327 Stage B) — bench-verified, run 332 (AE-on scenario)

Same shape as the IMX296 column above, with two real differences: IMX335's
native 2x2-binned output is a fixed 1296x972 (ISP input), ISP-CROPPED to
1280x960 (ISP output — the crop, `&isp`'s `crop-x0`/`crop-y0` = 8/6, lives in
the `innomaker_cam_imx335` SHIELD's own overlay, not a per-example one, since
it is a property of this sensor module — see `docs/camera-shields.md`'s Stage
B section for the derivation and `src/backends/camera/alif_isp_pico.c`'s
`BUILD_ASSERT` that proves it still produces 1280x960); and its AE envelope
is hal_alif patch 0014 (`imx335_ae_envelope.h`), capped at 30.0 dB analog
gain (a public Sony datasheet-flyer figure, not bench-derived the way
IMX296's 24.0 dB cap is). AWB/CCM stay on the stock ARX3A0 defaults, same as
IMX296 — colour is NOT calibrated (bench run 332 reports a green cast with
AWB off).

**Bench run 332** (E1M-AEN803 2026W36-0001, AE-on scenario): 60 frames, AE
converged (`ae_stable=1` at frame 50, Y mean 152), ISP output YUV420
1280x960 via the crop above, scene complete/straight/centred — the first
silicon confirmation of `out_form_rect`'s `left`/`top` crop offsets working
correctly on real hardware. Only error-class log line across the whole run:
one `FRAME_SEQ` event at stream start (the same single benign event every
other sensor's Stage B run also logs once). The AE-off scenario
(`-DEXTRA_CONF_FILE=overlay-no-ae.conf`) was NOT bench-run — still build-only.

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
