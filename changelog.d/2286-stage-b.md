### Added — full-FOV 2x2-binned 1280x960 mode in the OV5647 driver, Stage B (#2286)

Stage B of #2286: 1280x960 on E1M-AEN801/AEN803 + OV5647 now reads out the sensor's WHOLE
pixel array and bins it 2x2, instead of Stage A's ~49%-width centre crop at the same
output size. `ov5647_set_mode_regs()`'s new branch at `zephyr/drivers/video/ov5647.c:962`
("if (width == OV5647_MODE_1280X960_WIDTH && height == OV5647_MODE_1280X960_HEIGHT) {")
writes the new mode's window, output size, subsample/binning, analog, HTS and AEC
band-step registers as one coherent block, the same discipline AUTHORIZED LOCAL DIVERGENCE
#3 established for the 640x480 mode — binning must never be left armed against a different
window. Register values are re-derived from the RPi/OmniVision reference driver's OWN
2x2-binned mode (raspberrypi/linux rpi-6.12.y `drivers/media/i2c/ov5647.c`, the `reg_list`
its `ov5647_modes[]` names for a 1296x972 output) — cited by register meaning, no source
text copied (GPL) — then cropped further, to the delivered 1280x960, via the ISP offset
registers 0x3811/0x3813, `OV5647_BINNED1280_X_OFFSET` at `zephyr/drivers/video/ov5647.c:501`
("#define OV5647_BINNED1280_X_OFFSET 16"), the first mode in this driver to write them
explicitly rather than rely on their power-on default.

**PLL stays global**, per the issue: this driver keeps one `ov5647_init_regs[]` PLL write
(58,333,333, run 61's bench-proven value) for every mode. The reference's own HTS for this
mode, `OV5647_HTS_1280X960_BINNED` at `zephyr/drivers/video/ov5647.c:526`
("#define OV5647_HTS_1280X960_BINNED 1896 /* 0x0768 */"), is used unscaled despite the PLL
difference — the arithmetic happens to cancel (58,333,333 = 87,500,000 × 2/3 and 1896 =
2844 × 2/3, both exactly), landing within 0.02% of the reference's own line time, so the
AEC 50/60 Hz band-step line counts are also the reference's own 296/246, effectively
unscaled. `ov5647_hts_for()` at `zephyr/drivers/video/ov5647.c:868`
("static uint32_t ov5647_hts_for(uint32_t width, uint32_t height)") now returns this HTS
for 1280x960, which both the frame-rate math below and, once #2277 lands, the exposure
clamp depend on.

**Mode selection**: 1280x960 becomes the binned mode outright, not an opt-in alongside the
old crop — the crop delivered identical output pixels from a narrower FOV and a slower
line time (HTS 2700 vs 1896), so the binned mode strictly dominates it at this exact size.
The generic crop path stays available, unchanged, at every other size.

The mode also reaches 30 fps (VTS 1025 at HTS 1896, vs 15 fps's VTS 2051) — 45 fps is
rejected (VTS 683 < the 984-line minimum blanking). `examples/connectivity/camera-mjpeg-stream`
still requests 15 fps by default; raising it needs its own bench pass. `SENSOR_CTRL09` for
this mode (0x3709, undocumented) is not in the reference's own reg_list at all; it reuses
the 1:1 value by register grouping (0x52 only ever pairs with 640x480's heavier subsample)
and is flagged BENCH-UNVERIFIED, same honesty convention as the crop path's own analog
values. NO BENCH EVIDENCE YET for this mode — register values are re-derived, not measured
on silicon; the driver's file header and this changelog entry will be updated once a bench
run streams it.

`tests/zephyr/video_sensors/src/ov5647_test.c` gains `test_set_format_1280x960_binned_
fullfov_before_park` (the coherent register block, ordered before the lane park, mirroring
the existing 640x480 test), `test_set_format_1280x960_binned_30fps_vts` and
`test_enum_frmival_1280x960_accepts_30_rejects_45` (the 15/30 fps VTS math and the 30/45
fps reachability boundary); `test_set_format_switch_never_leaves_binning_on_crop_window`
and `test_crop_aec_max_bands_computed_per_mode`, which previously exercised the generic
crop path at 1280x960, now use 1920x1080 instead, since 1280x960 no longer takes that path.
All three new tests fail against the pre-Stage-B driver (native_sim twister,
`alp_sdk.video.rpi_csi2_sensors`: 39/42 passed, 3 failed with the driver reverted).

`examples/connectivity/camera-mjpeg-stream`'s `Kconfig`, `README.md`,
`boards/overlay-1280x960.conf` and `src/main.c` are updated to describe the new full-FOV
binned mode in place of Stage A's centre-crop description — output size and the 15 fps
default request are unchanged; only the field of view and the per-mode register table
changed. Existing bench runs 242/243 in the README were measured under Stage A's crop mode
and are labelled as such; this driver-mode change has not itself been re-benched (unit
E1M-AEN803 2026W36-0001, no new bench claims in this change).

Builds 0 warnings under `-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y` for
`camera-mjpeg-stream` (640x480 and the 1280x960 overlay), `aen-isp-ov5647-viewfinder` and
`aen-camera-firstlight` on `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he`.
