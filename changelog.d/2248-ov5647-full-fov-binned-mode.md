### Fixed — OV5647: 640x480 now matches the RPi/OmniVision reference driver's own tables, not an Alif-derived mix (#2248)

`zephyr/drivers/video/ov5647.c`'s 640x480 was a 648x488 1:1 centre crop -- a heavy telephoto
crop over roughly 25% of the array width -- not the full-array subsampled+binned 640x480 mode.
**Source of truth is the RPi/OmniVision reference driver**: raspberrypi/linux branch rpi-6.6.y,
`drivers/media/i2c/ov5647.c`, `ov5647_common_regs[]` and `ov5647_640x480_10bpp[]` -- bench run 61
applied those tables verbatim (minus `0x0100`/`0x0103`, which this driver's own park/reset
sequence owns) and streamed cleanly: RX-DDR clock 145833332, hsfrequency bin `{300, 0x14}`,
"[stg] 75 regs 0 failed", MFR start `ALP_OK`, zero `E:` lines / CSI fatals, frame CRC-matched
against a real scene (E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2, DesignWare CSI-2
RX, RAW10).

An earlier pass (bench runs 56/58/60) reached the same shape -- a full-FOV binned 640x480 mode
plus a common analog/BLC/AEC init -- but sourced its register values from Alif's own OV5647 table
for this exact silicon (`alif-dfp-ref components/Source/OV5647_camera_sensor.c:118-136`), which
is a **variant** of the RPi/OmniVision reference, not an independent source: it shares most
analog/BLC/AEC values with the reference but diverges at `0x3821` (Alif `0x07` vs the reference's
`0x01`) and omits the ISP block enables entirely. Running Alif's `0x3821 = 0x07` analog-timing
variant on top of the older, full-resolution-era PLL (`OV5647_PLL_MULT = 105`, see below) is what
produced visible vertical stripes runs 56/58/60 bench-proved as "clean" against the wrong
baseline. Run 61 replaces that mix wholesale with the reference values rather than patching it
further; column fixed-pattern noise fell from 6-8 LSB (3.7-5.9% of full scale) per Bayer plane
with the runs 56/58/60 mix to 1.0-1.5 LSB (~1.1%) with the reference set, same dark lab, same
0.51 s x15.5 gain exposure -- the visible stripes are gone.

**PLL**: `OV5647_PLL_MULT` moves from 105 (run 52's fix, matched to mainline's declared constants
for its FULL-RESOLUTION mode) to 70 (`0x46`, run 61, matched to the reference's own 640x480 10bpp
table): VCO 25 MHz / 3 * 70 = 583.33 MHz, lane bit rate 291.67 Mbps/lane, `OV5647_PIXEL_RATE`
58333333 (matching the reference's declared 58333000 to within ~0.0006% integer-truncation
rounding; the receiver's measured RX-DDR clock 145833332 = 58333000 * 10 / (2 * 2) confirms the
lock). This is deliberately ONE global PLL for both the 640x480 binned mode and the crop path --
the crop path now also runs at 291.67 Mbps/lane, slower than mainline's own full-resolution PLL,
which is safe (a lower bit rate cannot exceed the receiver's timing budget) but UNVERIFIED against
mainline's actual crop-mode PLL constants; only 640x480 has bench evidence for this exact PLL.

**HTS is now per mode**, not one driver-wide constant: the 640x480 binned mode writes the
reference's `0x380c`/`0x380d` = `0x073c` (1852); the crop path keeps the driver's own
bench-proven `2700` (run 52) rather than mainline's full-resolution `2844`, which is unverified
here. `ov5647_frmrate_to_vts()`, `ov5647_enum_frmival()` and `ov5647_set_frmival()` all now derive
HTS from the active mode via a new `ov5647_hts_for()` helper instead of a single `OV5647_HTS`
constant.

**Orientation**: `0x3821` bits[2:1] are this driver's existing `OV5647_TC_REG21_MIRROR` mask
(horizontal mirror). `0x01` (this fix, bits[2:1] clear) vs `0x07` (runs 56/58/60, Alif's variant,
bits[2:1] set) produced a LEFT-RIGHT-MIRRORED scene on the bench at `0x07` -- `0x01` is correct.
Per the RPi driver, `0x3821 = 0x01` with `0x3820 = 0x41` is its hflip=1/vflip=0 configuration,
paired with `MEDIA_BUS_FMT_SBGGR10_1X10` -- matching this driver's advertised SBGGR10P/SBGGR8
formats. Absolute orientation (which edge of the frame is physically "up") is still UNCONFIRMED
against a known real scene; only left-right mirroring relative to the two candidate register
values has bench evidence.

`ov5647_set_mode_regs()` picks the full-FOV binned register set (window, output size, subsample
`0x3814`/`0x3815` = `0x35`, binning `0x3820`/`0x3821` = `0x41`/`0x01`, binned-mode analog
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x59`/`0x00`/`0x64`/`0x52`, HTS `0x073c`) for exactly
640x480; every other size keeps the existing centred crop, but the crop path now also explicitly
re-asserts the 1:1 subsample/binning/analog/HTS values (`0x3814`/`0x3815` = `0x11`,
`0x3820`/`0x3821` = `0x40`/`0x00`, HTS `2700`, plus mainline's full-resolution analog values
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x5b`/`0x04`/`0x64`/`0x12`, cited from mainline and
BENCH-UNVERIFIED on this module) -- so a prior 640x480 selection can never leave binning (or its
HTS) armed on a later crop window. This closes the ordering trap bench run 54 hit: applying the
binning registers and then letting `ov5647_set_window()` rewrite the window to the crop made the
sensor emit short lines against a 640-pixel frame declaration, and the CSI host raised "Fatal
Interrupt due to mismatch of Frame Start and Frame End" on VC0 44 times in 2 s with no image
delivered. The window, output size, subsample, binning and HTS registers are now always written
as one coherent set per mode.

**Common init** now replaces runs 56/58/60's Alif-derived block with the reference's
`ov5647_common_regs[]`, minus `0x0100`/`0x0103` (park/reset), minus `0x3017` (kept at OUR `0xf0`
-- AUTHORIZED LOCAL DIVERGENCE #1/#2, a deliberate divergence from both mainline's `0xe0` and the
reference driver), and minus `0x3503` (left to this driver's existing exposure-control logic).
Unlike runs 56/58/60, it **includes** the ISP block enables `0x5000 = 0x06`, `0x5003 = 0x08`,
`0x5a00 = 0x08` -- run 61 streamed clean with them. Run 58's CSI "incorrect frame sequence"
fatals, previously blamed on ISP enables as a category, actually came from Alif's LARGER ISP set,
which additionally wrote `0x5001`/`0x5002` and (in the BLC section) `0x4050`/`0x4051` -- none of
those four are in the reference table and none are written here; do not add them back.
`0x3018` (MIPI PHY/route control) is now a literal reference byte (`0x44`) written as part of
this block, superseding the previous targeted `PHY_PD_MIPI`/`PHY_PD_LPRX`/`MIPI_EN` bitfield
modify.

`OV5647_EXPOSURE_DEFAULT` (unchanged by run 61) still moves from `0x20` (2 lines -- effectively a
closed shutter for a manual-exposure user) to `0x0FFF` (~256 lines), matching the order of
magnitude of Alif's own shipped default (~`0x000FFF`) for this silicon; the AEC gain/limit
registers themselves are left unchanged. The existing `VIDEO_CID_ANALOGUE_GAIN` ctrl default is
`0` -- checked, and left as-is: auto-gain (`VIDEO_CID_AUTOGAIN`) defaults on, so the manual gain
value only takes effect once a caller explicitly disables auto-gain, the same gating
`VIDEO_CID_EXPOSURE_AUTO` already applies to `OV5647_EXPOSURE_DEFAULT`.

**Low light**: at the default 15 fps VTS in a dark lab the AEC railed at both its limits (502
lines = 2x the `0x3a0b` band step; gain at the `0x3a19` ceiling) and a real image needed ~16000
lines (0.49 s) -- a scene limitation, not a driver bug; the AEC limit registers are unchanged.

The vendored driver's RETIREMENT note at `zephyr/drivers/video/ov5647.c:165`
("DIVERGENCE #1 (the LP-11 lane park), DIVERGENCE #2 (the") now requires all three fixes -- lane
park, PLL + MIPI-TX pad-drive init, and this full-FOV/per-mode-HTS/common-init/exposure-default
change -- to be confirmed present upstream before the vendored copy can be deleted.

New `tests/zephyr/video_sensors/src/ov5647_test.c` coverage: the 640x480 binned set (window,
subsample, binning enable, reference orientation `0x01`, per-mode HTS `0x073c`) landing before
the lane park, the run-54 ordering trap (640x480 -> another size -> back never leaves binning or
its HTS armed on a crop window; the crop path writes HTS `2700`), and the common analog/BLC/AEC
init landing before the first running `MODE_SELECT`. Each assertion was proven to fail against a
deliberately broken driver before being confirmed against the fix. `docs/camera-shields.md`'s
OV5647 row and driver section are updated to match.
