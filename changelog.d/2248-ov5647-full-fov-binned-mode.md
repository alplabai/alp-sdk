### Fixed — OV5647: 640x480 is now the sensor's real full-array binned mode, not a 25%-of-array crop (#2248)

`zephyr/drivers/video/ov5647.c`'s 640x480 was a 648x488 1:1 centre crop -- a heavy telephoto
crop over roughly 25% of the array width -- not the full-array subsampled+binned 640x480 both
Alif's own validated table for this exact silicon and mainline Linux use. Bench runs 56 and 60
(E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2, DesignWare CSI-2 RX, RAW10) proved the
full-FOV register set -- full-array window, subsample `0x3814`/`0x3815` = `0x35`, binning
`0x3821`/`0x3820` = `0x07`/`0x41`, binned-mode analog `0x3612`/`0x3618`/`0x3708`/`0x3709` =
`0x59`/`0x00`/`0x64`/`0x52` -- streams cleanly with a real, recognisable image, written in
software standby right after `ov5647_set_window()` and before the lane park.

`ov5647_set_mode_regs()` now picks this full-FOV binned set for exactly 640x480; every other
size keeps the existing centred crop, but the crop path now also explicitly re-asserts the 1:1
subsample/binning/analog values (`0x3814`/`0x3815` = `0x11`, `0x3820`/`0x3821` = `0x40`/`0x00`,
plus mainline's full-resolution analog values `0x3612`/`0x3618`/`0x3708`/`0x3709` =
`0x5b`/`0x04`/`0x64`/`0x12`, cited from mainline and BENCH-UNVERIFIED on this module) -- so a
prior 640x480 selection can never leave binning armed on a later crop window. This closes the
ordering trap bench run 54 hit: applying the binning registers and then letting
`ov5647_set_window()` rewrite the window to the crop made the sensor emit short lines against a
640-pixel frame declaration, and the CSI host raised "Fatal Interrupt due to mismatch of Frame
Start and Frame End" on VC0 44 times in 2 s with no image delivered. The window, output size,
subsample and binning registers are now always written as one coherent set per mode.

Bench run 58 (phases PC/PD/PF, 0 register read-back mismatches, clean streaming) also added the
common analog bias / BLC / AEC registers mainline writes in `ov5647_common_regs[]` for every
mode and this driver never had, appended to `ov5647_init_regs[]`. Deliberately **not** added:
the ISP block enables (`0x5000`..`0x5003`/`0x5a00`) -- run 58 phase PB, the only phase that added
them, is the phase that threw CSI "incorrect frame sequence" fatals at stream-on; the ISP is
left at its power-on state.

`OV5647_EXPOSURE_DEFAULT` moves from `0x20` (2 lines -- effectively a closed shutter for a
manual-exposure user) to `0x0FFF` (~256 lines), matching the order of magnitude of Alif's own
shipped default (~`0x000FFF`) for this silicon; the AEC gain/limit registers themselves are left
unchanged. The existing `VIDEO_CID_ANALOGUE_GAIN` ctrl default is `0` -- checked, and left as-is:
auto-gain (`VIDEO_CID_AUTOGAIN`) defaults on, so the manual gain value only takes effect once a
caller explicitly disables auto-gain, the same gating `VIDEO_CID_EXPOSURE_AUTO` already applies
to `OV5647_EXPOSURE_DEFAULT`.

**Low light**: at the default 15 fps VTS in a dark lab the AEC railed at both its limits (502
lines = 2x the `0x3a0b` band step; gain at the `0x3a19` ceiling) and a real image needed ~16000
lines (0.49 s) -- a scene limitation, not a driver bug; the AEC limit registers are unchanged.
Raw-frame stripes seen when viewing a capture as greyscale are (a) the Bayer colour-filter
mosaic itself and (b) column fixed-pattern noise that scales with analog gain -- roughly 0.8-1.1
LSB standard deviation at x2 gain and 6-8 LSB at x15.5 gain on this module (about 0.7% of full
scale at maximum gain) -- normal sensor behaviour, only visually dominant at high gain in low
light.

The vendored driver's RETIREMENT note at `zephyr/drivers/video/ov5647.c:126-129`
("DIVERGENCE #1 (the LP-11 lane park), DIVERGENCE #2 (the") now requires all three fixes --
lane park, PLL + MIPI-TX pad-drive init, and this full-FOV/common-init/exposure-default change
-- to be confirmed present upstream before the vendored copy can be deleted.

New `tests/zephyr/video_sensors/src/ov5647_test.c` coverage: the 640x480 binned set landing
before the lane park, the run-54 ordering trap (640x480 -> another size -> back never leaves
binning armed on a crop window), and the common analog/BLC/AEC init landing before the first
running `MODE_SELECT`. Each was proven to fail against a deliberately broken driver before being
confirmed against the fix. `docs/camera-shields.md`'s OV5647 row and driver section are updated
to match.
