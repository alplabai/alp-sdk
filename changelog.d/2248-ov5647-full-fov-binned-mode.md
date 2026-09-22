### Fixed — OV5647: 640x480 now matches the RPi/OmniVision reference driver's own values, not an Alif-derived mix; default frame rate was silently 10 fps, not 15 (#2248)

`zephyr/drivers/video/ov5647.c`'s 640x480 was a 648x488 1:1 centre crop -- a heavy telephoto
crop over roughly 25% of the array width -- not the full-array subsampled+binned 640x480 mode.
The register VALUES this fix programs are taken as hardware facts from the RPi/OmniVision
reference driver: raspberrypi/linux branch rpi-6.6.y, `drivers/media/i2c/ov5647.c`,
`ov5647_common_regs[]` and `ov5647_640x480_10bpp[]` (GPL-2.0 -- this file copies no source text
from that driver, only numeric register addresses/values, which are hardware facts about the
silicon, not expression). Bench run 61 ran those tables closely but NOT byte-identically:
`0x0100`/`0x0103` are owned by this driver's own reset/park sequence, not the table; the table's
`0x3017 = 0xe0` was written but this driver's re-park step (AUTHORIZED LOCAL DIVERGENCE #1) then
overwrote it to OUR `0xf0` before streaming, so the stream always started with `0xf0`; `0x3503`
was left to the app's own exposure control rather than the table's `0x03`; and `0x4800` used
this driver's park/stream-on sequence value (`0x04`), not the table's `0x34`. Run 61 streamed
clean: "[stg] 75 regs 0 failed", MFR start `ALP_OK`, zero `E:` lines / CSI fatals, frame
CRC-matched against a real scene (E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2,
DesignWare CSI-2 RX, RAW10), at the CSI-2 receiver's matched hsfrequency bin 20 (`{300, 0x14}`)
for the corrected PLL -- that bin match, not any receiver-side clock readback, is the PLL-lock
evidence; a receiver register that merely echoes our own declared `VIDEO_CID_PIXEL_RATE` back
through `video_get_csi_link_freq()` is not independent confirmation and is not cited as one.

**Run 62 bench-verified the COMMITTED driver itself**, not a modified bench app: the test app
wrote no mode registers and read back 77 registers against the reference, 0 mismatches; it
streamed with zero `E:` lines; column fixed-pattern noise matched run 61 plane by plane (1.65 /
1.19 / 1.18 / 0.97 LSB, ~1.1% of signal). The maintainer then viewed a host-demosaiced colour
render of the run-62 frame and **confirmed it is the correct image**: orientation
`0x3821 = 0x01` / `0x3820 = 0x41` is unmirrored and correct, Bayer order BGGR, green channels
diagonal and equal (107.2 / 106.8) -- maintainer-confirmed on run 62's actual rendered frame, not
inferred from register semantics alone.

An earlier pass (bench runs 56/58/60) reached the same shape -- a full-FOV binned 640x480 mode
plus a common analog/BLC/AEC init -- but sourced its register values from Alif's own OV5647 table
for this exact silicon (Alif Ensemble CMSIS-DFP package "AlifSemiconductor.Ensemble", package
v2.1.0, `components/Source/OV5647_camera_sensor.c` around lines 118-136), which is a **variant** of
the RPi/OmniVision reference, not an independent source: it shares most analog/BLC/AEC values
with the reference but diverges at `0x3821` (Alif `0x07` vs the reference's `0x01` -- bits[2:1]
of `0x3821` are this driver's horizontal-mirror mask, NOT "analog timing") and writes a
DIFFERENT ISP-enable set (`0x5000=0x06, 0x5001=0x01, 0x5002=0x41, 0x5003=0x08, 0x5a00=0x08` --
Alif's table does NOT omit the ISP enables, it writes a larger set than the reference's
`0x5000`/`0x5003`/`0x5a00`).

**WHICH of runs 56/58/60's several changes vs runs 61/62 (PLL, HTS, `0x3821`, the ISP-enable
set, and `0x3000..0x3002`) actually caused the visible vertical stripes runs 56/58/60
bench-proved as "clean" is NOT ESTABLISHED** -- run 61 changed all of them together against the
reference, not one at a time. Runs 56/58/60 ran Alif's `0x3821 = 0x07` on `OV5647_PLL_MULT =
105`, the SAME PLL run 52 originally fixed -- not run 61's corrected 70 -- so the stripes cannot
be attributed to a PLL/`0x3821` interaction specifically without more bisection than this
investigation did. What runs 61/62 DO establish is the outcome: the reference register set,
applied together, streams clean with the column-FPN numbers below; column fixed-pattern noise
fell from 6-8 LSB (3.7-5.9% of full scale) per Bayer plane with the runs 56/58/60 mix to 1.0-1.5
LSB (~1.1%) with the reference set, same dark lab, same 0.51 s x15.5 gain exposure -- the
visible stripes are gone, but that is the basis for shipping the reference set, not a claim
about which single register mattered most.

**PLL**: `OV5647_PLL_MULT` moves from 105 (run 52's fix, matched to mainline's declared constants
for its FULL-RESOLUTION mode) to 70 (`0x46`, run 61, matched to the reference's own 640x480 10bpp
table): VCO 25 MHz / 3 * 70 = 583.33 MHz, lane bit rate 291.67 Mbps/lane, `OV5647_PIXEL_RATE`
58333333 (matching the reference's declared 58333000 to within ~0.0006% integer-truncation
rounding). This is deliberately ONE global PLL for both the 640x480 binned mode and the crop path
-- the crop path now also runs at 291.67 Mbps/lane, slower than mainline's own full-resolution
PLL, which is safe (a lower bit rate cannot exceed the receiver's timing budget) but UNVERIFIED
against mainline's actual crop-mode PLL constants; only 640x480 has bench evidence for this exact
PLL.

**HTS is now per mode**, not one driver-wide constant: the 640x480 binned mode writes the
reference's `0x380c`/`0x380d` = `0x073c` (1852); the crop path keeps the driver's own
bench-proven `2700` (run 52) rather than mainline's full-resolution `2844`, which is unverified
here. `ov5647_frmrate_to_vts()`, `ov5647_enum_frmival()` and `ov5647_set_frmival()` all now derive
HTS from the active mode via a new `ov5647_hts_for()` helper instead of a single `OV5647_HTS`
constant.

**MAJOR BUG, fixed by bench run 62: the default frame rate at 640x480 was silently 10 fps, not
15.** `ov5647_init()` used to boot into the full-resolution crop (`OV5647_FULL_WIDTH`/`HEIGHT`);
at that height and the crop-path HTS (2700), the driver's own default 15 fps needs VTS 1440,
below `1944 + 24 = 1968`, so it was unreachable. Zephyr's `video_closest_frmival()` stops
enumerating at the first unreachable (sorted-ascending) rate, so it never even looked past 10
fps -- and that 10 fps then stuck in `data->frmrate` for every LATER `ov5647_set_fmt()` call,
including the 640x480 mode this driver actually ships, since nothing in `src/backends/camera` or
`examples/aen/aen-camera-firstlight` ever calls `set_frmival` to override it. `ov5647_init()` now
boots directly into 640x480 (the mode this driver actually ships, and the mode issue #2248 has
bench evidence for), where 15 fps IS reachable (VTS 2099/`0x0833` >= `480 + 24`), so the shipped
default is correctly 15. A caller that explicitly switches to a full-resolution crop still gets a
valid, if lower, rate -- HTS 2700 genuinely cannot sustain 15 fps at 1944 lines; that is a real
PLL/line-time limit, not a bug, and `video_closest_frmival()` correctly falls back to the next
reachable entry in that case.

**Orientation (maintainer-confirmed, run 62)**: `0x3821` bits[2:1] are this driver's existing
`OV5647_TC_REG21_MIRROR` mask (horizontal mirror). `0x01` (this fix, bits[2:1] clear) vs `0x07`
(runs 56/58/60, Alif's variant, bits[2:1] set) produced a LEFT-RIGHT-MIRRORED scene on the bench
at `0x07` -- `0x01` is correct and unmirrored, as confirmed by the maintainer's review of a
host-demosaiced run-62 frame (BGGR, greens diagonal and equal). Per the RPi driver,
`0x3821 = 0x01` with `0x3820 = 0x41` is its hflip=1/vflip=0 configuration, paired with
`MEDIA_BUS_FMT_SBGGR10_1X10` -- matching this driver's advertised SBGGR10P/SBGGR8 formats.

`ov5647_set_mode_regs()` picks the full-FOV binned register set (window, output size, subsample
`0x3814`/`0x3815` = `0x35`, binning `0x3820`/`0x3821` = `0x41`/`0x01`, binned-mode analog
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x59`/`0x00`/`0x64`/`0x52`, HTS `0x073c`, and the 50/60 Hz
AEC band step `0x3a09`/`0x3a0a`/`0x3a0b`/`0x3a0d`/`0x3a0e` and `0x4004` -- **moved here from the
common init**, since the band step is in LINES, which depends on line time/HTS, and a value
correct for this mode's HTS is wrong for the crop path's) for exactly 640x480; every other size
keeps the existing centred crop, but the crop path now also explicitly re-asserts the 1:1
subsample/binning/analog/HTS/band-step values (`0x3814`/`0x3815` = `0x11`, `0x3820`/`0x3821` =
`0x40`/`0x00`, HTS `2700`, mainline's full-resolution analog values
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x5b`/`0x04`/`0x64`/`0x12`, cited from mainline and
BENCH-UNVERIFIED on this module, and the SAME AEC band-step values as the binned mode -- no
mainline full-resolution citation is available, so reused rather than left unwritten, which
would silently retain a prior mode's stale line counts; BENCH-UNVERIFIED at this HTS) -- so a
prior 640x480 selection can never leave binning (or its HTS/band step) armed on a later crop
window. This closes the ordering trap bench run 54 hit: applying the binning registers and then
letting `ov5647_set_window()` rewrite the window to the crop made the sensor emit short lines
against a 640-pixel frame declaration, and the CSI host raised "Fatal Interrupt due to mismatch
of Frame Start and Frame End" on VC0 44 times in 2 s with no image delivered. The window, output
size, subsample, binning, HTS and AEC-band-step registers are now always written as one coherent
set per mode.

**Common init** register VALUES match runs 56/58/60's Alif-derived block for most addresses, but
the SOURCE is now the reference's `ov5647_common_regs[]`, minus `0x0100`/`0x0103` (park/reset),
minus `0x3017` (kept at OUR `0xf0` -- AUTHORIZED LOCAL DIVERGENCE #1/#2, a deliberate divergence
from both mainline's `0xe0` and the reference driver's table value, which this driver's re-park
overwrites before streaming regardless), minus `0x3503` (left to this driver's existing
exposure-control logic), and minus the mode-specific AEC band-step registers (moved to
`ov5647_set_mode_regs()`, see above). UNLIKE runs 56/58/60, it **includes** the reference's ISP
block enables `0x5000 = 0x06`, `0x5003 = 0x08`, `0x5a00 = 0x08` -- run 61 streamed clean with
them. Run 58's CSI "incorrect frame sequence" fatals, previously blamed on ISP enables as a
category, actually came from Alif's DIFFERENT ISP set specifically, which additionally writes
`0x5001`/`0x5002` and (in runs 56/58/60's now-superseded BLC section) `0x4050`/`0x4051` -- none
of those four are in the reference table and none are written here; do not add them back.
`0x3000`/`0x3001`/`0x3002` = `0x00` and `0x3018` (MIPI PHY/route control) are now literal
reference bytes (`0x00`/`0x00`/`0x00`, `0x44`) written as part of this block, the latter
superseding the previous targeted `PHY_PD_MIPI`/`PHY_PD_LPRX`/`MIPI_EN` bitfield modify.

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
The OV5647 is a colour Bayer sensor; the driver delivers RAW10 BGGR, and demosaic/colour
reconstruction is a downstream ISP job (see run 62's maintainer-confirmed colour render above) --
raw frames viewed as grey show the Bayer mosaic as fine stripes, which is expected and not the
same phenomenon as the column fixed-pattern noise discussed above.

The vendored driver's RETIREMENT note at `zephyr/drivers/video/ov5647.c:195`
("DIVERGENCE #1 (the LP-11 lane park), DIVERGENCE #2 (the") now requires all three fixes -- lane
park, PLL + MIPI-TX pad-drive init, and this full-FOV/per-mode-HTS/AEC-band-step/common-init/
exposure-default/default-frame-rate change -- to be confirmed present upstream before the
vendored copy can be deleted.

New `tests/zephyr/video_sensors/src/ov5647_test.c` coverage: the full 640x480 binned register
block (window, output size, subsample, binning enable, reference orientation `0x01`, per-mode
HTS `0x073c`, binned-mode analog, and the AEC band step) landing after the `0x3034` bit-mode
write and before the lane park, the default-frame-rate regression (640x480's default VTS is the
15 fps value, not the 10 fps one), `ov5647_enum_frmival()` accepting 60 fps and rejecting 90 fps
at 640x480, the run-54 ordering trap (640x480 -> another size -> back never leaves binning, its
HTS or its AEC band step armed on a crop window; the crop path computes VTS with HTS 2700), an
EXHAUSTIVE table-driven check of every common-init register (not a 3-register sample), and a
guard that `0x5001`, `0x5002`, `0x4050` and `0x4051` are never written. Each assertion was proven
to fail against a deliberately broken driver before being confirmed against the fix.
`docs/camera-shields.md`'s OV5647 row and driver section are updated to match.
