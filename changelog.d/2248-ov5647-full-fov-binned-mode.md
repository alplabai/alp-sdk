### Fixed — OV5647: 640x480 now matches the RPi/OmniVision reference driver's own values, not an Alif-derived mix; five pre-merge behaviour bugs caught and fixed before merge (#2248)

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
DesignWare CSI-2 RX, RAW10). The CSI-2 receiver's hsfrequency bin 20 (`{300, 0x14}`) is
CONFIGURATION, not evidence: it is computed from our own declared `VIDEO_CID_PIXEL_RATE` via
`video_get_csi_link_freq()`, so it follows from what we declared regardless of what the silicon's
real PLL is doing. The actual evidence is that runs 61/62/63 (below) streamed with ZERO CSI
fatals while the receiver was configured for that bin -- a genuine PLL/bin mismatch shows up as
`ERRSOTSYNCHS` or `PHY_FATAL`, not silent success.

**Run 62 bench-verified the COMMITTED driver itself**, not a modified bench app: the test app
wrote no mode registers and read back 77 registers against the reference, 0 mismatches; it
streamed with zero `E:` lines; column fixed-pattern noise matched run 61 plane by plane (1.65 /
1.19 / 1.18 / 0.97 LSB, ~1.1% of signal). The maintainer then viewed a host-demosaiced colour
render of the run-62 frame and **confirmed it is the correct image**: orientation
`0x3821 = 0x01` / `0x3820 = 0x41` is unmirrored and correct, Bayer order BGGR, green channels
diagonal and equal (107.2 / 106.8) -- maintainer-confirmed on run 62's actual rendered frame, not
inferred from register semantics alone. **Run 63 re-verified the committed fix** (VTS `0x0833`,
`fps_x100=1501`, 77/0 register mismatches, column FPN unchanged from run 62) and its review
mutation-tested the fix-up, finding three further real behaviour bugs (all below) plus the
accuracy corrections this entry folds in.

An earlier pass (bench runs 56/58/60) reached the same shape -- a full-FOV binned 640x480 mode
plus a common analog/BLC/AEC init -- but sourced its register values from Alif's own OV5647 table
for this exact silicon (Alif Ensemble CMSIS-DFP package "AlifSemiconductor.Ensemble", package
v2.1.0, `components/Source/OV5647_camera_sensor.c` around lines 118-136), which is a **variant**
of the RPi/OmniVision reference, not an independent source: it shares most analog/BLC/AEC values
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
investigation did. What runs 61/62/63 DO establish is the outcome: the reference register set,
applied together, streams clean with the column-FPN numbers below; column fixed-pattern noise
fell from 6-8 LSB (3.7-5.9% of signal) per Bayer plane with the runs 56/58/60 mix to 1.0-1.5 LSB
(~1.1% of signal, plane mean ~107) with the reference set, same dark lab, same 0.51 s x15.5 gain
exposure -- the visible stripes are gone, but that is the basis for shipping the reference set,
not a claim about which single register mattered most.

**PLL**: `OV5647_PLL_MULT` moves from 105 (run 52's fix, matched to mainline's declared constants
for its FULL-RESOLUTION mode) to 70 (`0x46`, run 61, matched to the reference's own 640x480 10bpp
table): VCO 25 MHz / 3 * 70 = 583.33 MHz, lane bit rate 291.67 Mbps/lane, `OV5647_PIXEL_RATE`
58333333. **Citation corrected (round 4)**: this used to be described as matching "the reference's
declared pixel_rate (58333000)" -- raspberrypi/linux's rpi-6.6.y, rpi-6.1.y and rpi-6.12.y all
declare `.pixel_rate = 55000000` for the 640x480 10bpp mode, not 58333000; no revision checked
declares 58333000 anywhere. `OV5647_PIXEL_RATE` is instead DERIVED from this driver's own PLL
constants and BENCH-MATCHED: run 63 measured `fps_x100=1501` at VTS `0x0833`/HTS 1852, exactly
what the driver's VTS formula gives from 58333333, not from 55000000. This is deliberately ONE
global PLL for both the 640x480 binned mode and the crop path
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

**PRE-MERGE REGRESSION, caught before merge, never shipped: the default frame rate at 640x480
was 10 fps, not 15.** The PLL retarget above lowered `cfg->pixel_rate` from 87500000 to 58333333
within this same unreleased PR chain, which silently broke `ov5647_init()`'s (then)
full-resolution-crop boot format: the driver's own default 15 fps became unreachable at that
height -- HTS 2700 needs VTS 1440, below `1944 + 24 = 1968`, where the pre-PLL-retarget
`pixel_rate` had made it reachable. Zephyr's `video_closest_frmival()` stops enumerating at the
first unreachable (sorted-ascending) rate, so it never even looked past 10 fps. Bench run 62
caught this before merge, in the same fix-up pass that also moved the boot format to 640x480 (a
genuine, independent improvement -- 640x480 is this driver's real, bench-proven default mode),
where 15 fps IS reachable (VTS 2099/`0x0833` >= `480 + 24`). A caller that explicitly switches to
a full-resolution crop still gets a valid, if lower, rate -- HTS 2700 genuinely cannot sustain 15
fps at 1944 lines; that is a real PLL/line-time limit, not a bug, and `video_closest_frmival()`
correctly falls back to the next reachable entry in that case.

**MAJOR BUG, fixed by run 63's review, before merge, never shipped: the frame rate could still
stick after a LATER format change**, distinct from the boot-time issue above.
`ov5647_set_fmt()` fed `data->frmrate` (the last EFFECTIVE, possibly clamped, rate) back in as
the new request on every format change: `set_format(2592x1944)` clamps to 10 fps (HTS 2700 can't
sustain 15 fps at that height), and a LATER `set_format(640x480)` inherited that clamped 10
rather than re-requesting 15, even though 640x480 can reach it fine. Fixed with a new
`data->requested_frmrate` field (see the ROUND-4 REGRESSION below for what this became), set only
by `ov5647_set_frmival()` (default 15) to the value the caller actually asked for;
`ov5647_set_fmt()` now re-requests `requested_frmrate` on every format change instead of the
clamped `frmrate`.

**MAJOR BUG, fixed by run 63's review, before merge, never shipped: format changes silently
undid the flip controls.** `ov5647_set_mode_regs()` writes `0x3820`/`0x3821` as whole bytes,
which wiped `OV5647_TC_REG20_VFLIP`/`OV5647_TC_REG21_MIRROR` bits a caller had set via
`VIDEO_CID_VFLIP`/`VIDEO_CID_HFLIP`, while the ctrl itself kept reporting the value the caller
set -- ctrl and hardware silently diverged. Fixed by re-applying both ctrls (via new
`ov5647_set_ctrl_hflip()`/`ov5647_set_ctrl_vflip()` helpers, also used by `ov5647_set_ctrl()`
itself) right after `ov5647_set_mode_regs()` inside `ov5647_set_fmt()`. With both ctrls at their
defaults (unset), the mode bytes are unaffected: `TC_REG20_BINNED`/`TC_REG21_BINNED`
(`0x41`/`0x01`) already equal exactly the maintainer-confirmed orientation with bits[2:1] clear,
so the fix merges in zero extra bits in the common case -- verified by a dedicated test, not just
asserted.

**ROUND-4 REGRESSION, caught by a review of the round-3 fix-up above, before merge, never
shipped: the sticky-rate fix itself dropped the requested frame rate's numerator.**
`data->requested_frmrate` was a bare `uint32_t` storing only `frmival->denominator`, so a request
whose numerator is not 1 -- e.g. `{2, 60}`, still 30 fps, the exact shape Zephyr's own `video
frmival <dev> <interval>` shell command sends (`100ms` is `{100, 1000}`, not `{1, 10}`) -- silently
turned into a DIFFERENT rate (`{1, 60}`, 60 fps) on the next format change. Fixed by widening the
field to the whole `struct video_frmival` (`requested_frmival`), and by moving the store to AFTER
`video_closest_frmival()` and the VTS write both succeed, so a rejected `set_frmival()` call no
longer overwrites a previously-saved good request either.

**ROUND-4 FIX-UP: the crop-path AEC band step fixed above was itself still wrong.** Copying
mainline's full-resolution line counts byte-for-byte assumes mainline's own 32.51us line time
(HTS 2844 / pixel_rate 87500000); this driver's crop path runs a 46.29us line (`OV5647_HTS_CROP`
2700 / `OV5647_PIXEL_RATE` 58333333). A band-step register is a line count standing in for a fixed
real-time AC period, so carrying a line count across a DIFFERENT line time changes the real time
it represents. Scaled by the line-time ratio instead: `0x3a08`/`0x3a09` = `0x00`/`0xd0` (208
lines, was 296), `0x3a0a`/`0x3a0b` = `0x00`/`0xad` (173 lines, was 246); `0x3a0d`/`0x3a0e`
(max bands per frame) recomputed via the reference's own floor(VTS/band) rule at VTS 1968
(`OV5647_FULL_HEIGHT + OV5647_VBLANK_MIN`, the same VTS mainline's own `0x08`/`0x06` reproduce
from) -- `0x0b`/`0x09`. Still BENCH-UNVERIFIED, not run on hardware.

**ROUND-4 (no NEW register writes, but an API contract change, not mere documentation): flipping
the sensor also flips the Bayer colour order, which `get_format()` did not report.** Translated
from the RPi/OmniVision reference's `ov5647_get_mbus_code()`: that driver's own `V4L2_CID_HFLIP`
is the logical inverse of the `0x3821` mirror bit, while this driver's `HFLIP` ctrl bit IS the
mirror bit directly -- so in terms of THIS driver's ctrl values, `(hflip, vflip)` maps `(0,0)` to
SBGGR (the maintainer-confirmed default), `(1,0)` to SGBRG, `(0,1)` to SGRBG, and `(1,1)` to
SRGGB. `get_format()` now derives and reports the flip-adjusted pixelformat. Only the default
order is bench/maintainer-verified; the three flipped orders are derived from the reference
mapping, not bench-checked on this module.

**ROUND-5, MAJOR BUG, caught by a review of round 4 above, before merge, never shipped:
`set_format()` could not accept back what `get_format()` had just reported.** Round 4 made
`get_format()` report the flip-shifted fourcc (e.g. `SGBRG10P` at `hflip=1`), but `set_format()`
still only matched the two BASE fourccs (`SBGGR8`/`SBGGR10P`) against `ov5647_fmts[]` via
`video_format_caps_index()` -- so a `get_format()` -> `set_format()` round trip, or any caller
(`video_alif.c`'s `alif_cam_get_fmt()`) that re-submits exactly what `get_format()` returned,
failed with `-ENOTSUP`. Fixed: `set_format()` now maps a request back to its base fourcc when it
equals what the CURRENT flip ctrls would derive from that base, and reports the flip-adjusted
fourcc back on success too, matching `get_format()`. A caller submitting a base fourcc directly
while flipped still works unchanged. `get_caps()` is still unchanged (a caller still requests the
base SBGGR8/SBGGR10P size/depth via `video_format_caps_index()`; the flip-adjusted order is a
derived side effect, not a separately advertised capability).

**ROUND-5, pre-existing: `set_frmival()` could echo a request it never actually applied.** When
`video_closest_frmival()` finds no candidate within `INT32_MAX` ns of the request (e.g. `{5, 1}`,
a 5 s interval, against this driver's fastest 10 fps candidate), it returns `0` without ever
updating its `match` out-param, leaving it at the caller's original raw request while the VTS
register is actually programmed for index 0 (10 fps) regardless. `ov5647_set_frmival()` used to
echo that stale raw request back to its own caller instead of the rate it just wrote to hardware.
Fixed: report `{1, ov5647_framerates[fie.index]}` -- the SAME construction `ov5647_enum_frmival()`
itself produces for that index, so this is a no-op on every request `video_closest_frmival()` DID
match, and only changes behaviour on this one previously-silent-mismatch path.

**ROUND-5: the crop-path max-bands-per-frame registers (`0x3a0d`/`0x3a0e`) were pinned to the
2592x1944 crop's own minimum-blanking VTS for every crop size.** A smaller crop -- e.g. 1280x960,
minimum-blanking VTS ~984 -- got the SAME `0x0b`/`0x09` (11/9 bands) as the 1944-line crop, even
though `11 * 173 = 1903` and `9 * 208 = 1872` both exceed its minimum-blanking VTS. Fixed:
computed per mode from the requested height's own minimum-blanking VTS (`floor(VTS/band)`, floored
at 1 band), the same rule the reference applies per its own modes -- see `ov5647_set_mode_regs()`'s
block comment. Still BENCH-UNVERIFIED, same as the band-step values themselves.

**ROUND-6, nit found by review, before merge, never shipped: `set_fmt()` only restored the
caller's requested pixelformat on the `video_format_caps_index()` failure path.** Every later
failure inside `set_fmt()` -- the `OV5647_MODE_SELECT`/`OV5647_SC_PLL_CTRL0` I2C writes,
`ov5647_set_mode_regs()`, `ov5647_set_ctrl_hflip()`/`ov5647_set_ctrl_vflip()`,
`ov5647_set_frmival()`, `ov5647_lane_park()` -- ran AFTER the round-5 flip-shifted-to-base remap
and returned with `fmt->pixelformat` still holding that internal BASE fourcc, not the caller's
original (possibly flip-shifted) request. Fixed: every failure path now `goto`s a single `err:`
label that restores `fmt->pixelformat` before returning, instead of restoring only ahead of the
one `-ENOTSUP` return. Also added: a RAW8 (`SGBRG8`) `get_format()` -> `set_format()` round trip
alongside the existing RAW10 one, and coverage for the restore itself -- an unsupported fourcc, an
unsupported size, and (the case that actually distinguishes "restored" from "never touched", since
the remap is a no-op at the default unflipped state) an unsupported size on a flip-shifted request.

**ROUND-4, test infrastructure: the suite's `common_init_regs[]` table was missing two registers
`ov5647_init_regs[]` already wrote** (`0x3503`/`OV5647_MANUAL_CTRL_VTS` and
`0x350c`/`0x350d`/`OV5647_VTS_DIFF`) -- a mutation test deleting either from the driver left every
test in the suite passing. Added, and a `ZTEST_SUITE` before-hook now resets both flip ctrls, the
frame interval, and the format ahead of EVERY test, replacing the in-test cleanup at the end of
`test_set_format_preserves_flip_ctrls` (which a failed assertion earlier in that same test would
have skipped). The emulator's write-log capacity (`OV5647_EMUL_LOG_CAPACITY`) is raised from 160
to 1024 to give that before-hook's extra writes headroom across the whole suite.

**Orientation (maintainer-confirmed, run 62)**: `0x3821` bits[2:1] are this driver's existing
`OV5647_TC_REG21_MIRROR` mask (horizontal mirror). `0x01` (this fix, bits[2:1] clear) vs `0x07`
(runs 56/58/60, Alif's variant, bits[2:1] set) produced a LEFT-RIGHT-MIRRORED scene on the bench
at `0x07` -- `0x01` is correct and unmirrored, as confirmed by the maintainer's review of a
host-demosaiced run-62 frame (BGGR, greens diagonal and equal). Per the RPi driver,
`0x3821 = 0x01` with `0x3820 = 0x41` is its hflip=1/vflip=0 configuration -- RPi's own `hflip`
ctrl sense is INVERTED relative to what "unmirrored" suggests here: their driver reports
`hflip=1` for the register state that is, on this silicon, the CORRECT unmirrored orientation,
not literally flipped. `0x3821 = 0x01` is paired with `MEDIA_BUS_FMT_SBGGR10_1X10` -- matching
this driver's advertised SBGGR10P/SBGGR8 formats.

`ov5647_set_mode_regs()` picks the full-FOV binned register set (window, output size, subsample
`0x3814`/`0x3815` = `0x35`, binning `0x3820`/`0x3821` = `0x41`/`0x01`, binned-mode analog
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x59`/`0x00`/`0x64`/`0x52`, HTS `0x073c`, and the 50/60 Hz
AEC band step `0x3a08`/`0x3a09`/`0x3a0a`/`0x3a0b`/`0x3a0d`/`0x3a0e` and `0x4004` -- **moved here
from the common init**, since the band step is in LINES, which depends on line time/HTS, and a
value correct for this mode's HTS is wrong for the crop path's; `0x3a08` moved for the same
reason even though its value happens to be `0x01` either way) for exactly 640x480; every other
size keeps the existing centred crop, but the crop path now also explicitly re-asserts the 1:1
subsample/binning/analog/HTS/band-step values (`0x3814`/`0x3815` = `0x11`, `0x3820`/`0x3821` =
`0x40`/`0x00`, HTS `2700`, mainline's full-resolution analog values
`0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x5b`/`0x04`/`0x64`/`0x12`, cited from mainline and
BENCH-UNVERIFIED on this module) **and the crop path's OWN AEC band step**, line-time-scaled from
mainline's full-resolution table (`0x3a08`/`0x3a09`/`0x3a0a`/`0x3a0b`/`0x3a0d`/`0x3a0e`/`0x4004` =
`0x00`/`0xd0`/`0x00`/`0xad`/`0x0b`/`0x09`/`0x04`, shown here for the 2592x1944 crop -- see the
ROUND-4 FIX-UP paragraph above for why and the arithmetic, and the ROUND-5 paragraph above for
why `0x3a0d`/`0x3a0e` are now computed per crop size rather than pinned to this one; still
BENCH-UNVERIFIED on this module) --

**MAJOR BUG, fixed by run 63's review, before merge, never shipped: the crop-path AEC band step
was wrong.** A previous version of this fix reused the VGA band-step numbers
(`0x3a0d=0x02`/`0x3a0e=0x01`, max bands per frame) on the crop path with the claim "no mainline
full-resolution citation is available" -- that claim was false. Reusing the VGA numbers on a
1944-line crop would have capped banding-mode AEC around a CALCULATED (not bench-measured) 502
lines. This was fixed first with mainline's own full-resolution values byte-for-byte, and that
fix-up was ITSELF found wrong by a round-4 review (see above). This closes the ordering
trap bench run 54 hit: applying the binning
registers and then letting `ov5647_set_window()` rewrite the window to the crop made the sensor
emit short lines against a 640-pixel frame declaration, and the CSI host raised "Fatal Interrupt
due to mismatch of Frame Start and Frame End" on VC0 44 times in 2 s with no image delivered. The
window, output size, subsample, binning, HTS and AEC-band-step registers are now always written
as one coherent set per mode.

**Common init** register VALUES match runs 56/58/60's Alif-derived block for most addresses, but
the SOURCE is now the reference's `ov5647_common_regs[]`, minus `0x0100`/`0x0103` (park/reset),
minus `0x3017` (kept at OUR `0xf0` -- AUTHORIZED LOCAL DIVERGENCE #1/#2, a deliberate divergence
from both mainline's `0xe0` and the reference driver's table value, which this driver's re-park
overwrites before streaming regardless), minus `0x3503` (left to this driver's existing
exposure-control logic), and minus the mode-specific AEC band-step registers, including `0x3a08`
(moved to `ov5647_set_mode_regs()`, see above). UNLIKE runs 56/58/60, it **includes** the
reference's ISP block enables `0x5000 = 0x06`, `0x5003 = 0x08`, `0x5a00 = 0x08` -- run 61
streamed clean with them. Run 58's CSI "incorrect frame sequence" fatals, previously blamed on
ISP enables as a category, actually came from Alif's DIFFERENT ISP set specifically, which
additionally writes `0x5001`/`0x5002` and (in runs 56/58/60's now-superseded BLC section)
`0x4050`/`0x4051` -- none of those four are in the reference table and none are written here; do
not add them back. `0x3000`/`0x3001`/`0x3002` = `0x00` and `0x3018` (MIPI PHY/route control) are
now literal reference bytes (`0x00`/`0x00`/`0x00`, `0x44`) written as part of this block, the
latter superseding the previous targeted `PHY_PD_MIPI`/`PHY_PD_LPRX`/`MIPI_EN` bitfield modify.

`OV5647_EXPOSURE_DEFAULT` (unchanged by run 61) still moves from `0x20` (2 lines -- effectively a
closed shutter for a manual-exposure user) to `0x0FFF` (~256 lines), matching the order of
magnitude of Alif's own shipped default (~`0x000FFF`) for this silicon; the AEC gain/limit
registers themselves are left unchanged. The existing `VIDEO_CID_ANALOGUE_GAIN` ctrl default is
`0` -- checked, and left as-is: auto-gain (`VIDEO_CID_AUTOGAIN`) defaults on, so the manual gain
value only takes effect once a caller explicitly disables auto-gain, the same gating
`VIDEO_CID_EXPOSURE_AUTO` already applies to `OV5647_EXPOSURE_DEFAULT`.

The default format before any `set_format()` call is now 640x480 SBGGR10P, not 2592x1944 --
`ov5647_init()`'s own boot format. The non-negotiated camera backend path allocates buffers from
this default, so this is user-visible for a caller that never calls `set_format()` itself.

**Low light**: at the default 15 fps VTS in a dark lab the AEC railed at both its limits (502
lines = 2x the `0x3a0b` band step; gain at the `0x3a19` ceiling) and a real image needed ~16000
lines (0.49 s) -- a scene limitation, not a driver bug; the AEC limit registers are unchanged.
The OV5647 is a colour Bayer sensor; the driver delivers RAW10 BGGR, and demosaic/colour
reconstruction is a downstream ISP job (see run 62's maintainer-confirmed colour render above) --
raw frames viewed as grey show the Bayer mosaic as fine stripes, which is expected and not the
same phenomenon as the column fixed-pattern noise discussed above.

The vendored driver's RETIREMENT note at `zephyr/drivers/video/ov5647.c:214`
("DIVERGENCE #1 (the LP-11 lane park), DIVERGENCE #2 (the") now requires all three fixes -- lane
park, PLL + MIPI-TX pad-drive init, and this full-FOV/per-mode-HTS/AEC-band-step/common-init/
exposure-default/frame-rate/flip-ctrl change -- to be confirmed present upstream before the
vendored copy can be deleted.

New `tests/zephyr/video_sensors/src/ov5647_test.c` coverage: the full 640x480 binned register
block (window, output size, subsample, binning enable, reference orientation `0x01`, per-mode
HTS `0x073c`, binned-mode analog, and the AEC band step) landing after the `0x3034` bit-mode
write and before the lane park; the default-frame-rate regression (640x480's default VTS is the
15 fps value, not the 10 fps one); `ov5647_enum_frmival()` accepting 60 fps and rejecting 90 fps
at 640x480; the run-54 ordering trap (640x480 -> another size -> back never leaves binning, its
HTS or its AEC band step armed on a crop window, now checked via the WRITE LOG on the crop side
too, not just a final-value readback a stale prior-mode value could coincidentally satisfy); the
frame rate not sticking at a prior mode's clamped value after a later format change, AND
preserving a numerator != 1 request across one (round 4); `VIDEO_CID_HFLIP`/`VIDEO_CID_VFLIP`
surviving a format change (and producing exactly `0x41`/`0x01` at their defaults); `get_format()`
reporting each of the four Bayer orders those ctrls produce (round 4); and an EXHAUSTIVE
table-driven check of every `ov5647_init_regs[]` common-init entry -- an earlier version of that
table checked only a subset and missed `0x303c`/`0x3106`/`0x301c` entirely (a mutation test
deleting any of them from the driver still passed every test in the suite); the table now also
covers `0x3016`/`0x301d`/`0x3017`, none of which
`test_pll_init_written_before_first_running_mode_select` checks (that test covers only the three
PLL divider values), and (round 4) `0x3503`/`0x350c`/`0x350d`, which the table had ALSO missed
even though the driver already wrote them. A guard that `0x5001`, `0x5002`, `0x4050` and `0x4051`
are never written, at any value. A `ZTEST_SUITE` before-hook (round 4) now resets both flip ctrls,
the frame interval, and the format ahead of every test, replacing per-test in-test cleanup that a
failed assertion earlier in the same test could skip; the emulator's write-log capacity is raised
from 160 to 1024 to give that hook's extra writes headroom. Every new and changed assertion in
this round was proven to fail against a deliberately broken driver before being confirmed against
the fix -- the earlier non-exhaustive common-init table is the counter-example that motivated
saying this precisely rather than as a blanket claim. `docs/camera-shields.md`'s OV5647 row and
driver section, and `docs/adr/0017-alp-sdk-over-the-vendor-sdk.md`, are updated to match.
