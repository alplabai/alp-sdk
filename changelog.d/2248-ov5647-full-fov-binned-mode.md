### Fixed — OV5647: 640x480 is now the full-array subsampled+binned mode, with register values taken from the RPi/OmniVision reference driver (#2248)

`zephyr/drivers/video/ov5647.c`'s 640x480 was a 648x488 1:1 centre crop -- a heavy telephoto
crop over roughly 25% of the array width -- not the full-array subsampled+binned 640x480 mode.
The register VALUES this fix programs are taken as hardware facts from the RPi/OmniVision
reference driver: raspberrypi/linux branch rpi-6.6.y, `drivers/media/i2c/ov5647.c`,
`ov5647_common_regs[]` and `ov5647_640x480_10bpp[]` (GPL-2.0 -- this file copies no source text
from that driver, only numeric register addresses/values, which are hardware facts about the
silicon, not expression). The tables are not applied byte-identically: `0x0100`/`0x0103` are
owned by this driver's own reset/park sequence; `0x3017` stays at this driver's `0xf0` (the
re-park step, AUTHORIZED LOCAL DIVERGENCE #1, writes it before streaming); `0x3503` is left to the
app's own exposure control; and `0x4800` uses this driver's park/stream-on value (`0x04`).

**Bench evidence** (E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2, DesignWare CSI-2 RX,
RAW10). Run 61 streamed clean: "[stg] 75 regs 0 failed", MFR start `ALP_OK`, zero `E:` lines / CSI
fatals, frame CRC-matched against a real scene. Run 62 verified the committed driver itself, not a
modified bench app: it wrote no mode registers and read back 77 registers against the reference,
0 mismatches; column fixed-pattern noise was 1.65 / 1.19 / 1.18 / 0.97 LSB per Bayer plane (~1.1%
of signal, plane mean ~107, 0.51 s x15.5 gain exposure), down from 6-8 LSB (3.7-5.9%) with an
earlier register mix sourced from Alif's own OV5647 table (Alif Ensemble CMSIS-DFP
`components/Source/OV5647_camera_sensor.c`), which is a variant of the reference rather than an
independent source. Which single register of that earlier mix caused the stripes is not
established. The maintainer confirmed a host-demosaiced colour render of the run-62 frame is the
correct image: orientation `0x3821 = 0x01` / `0x3820 = 0x41` is unmirrored, Bayer order BGGR, green
channels diagonal and equal (107.2 / 106.8). Run 63 re-verified the committed driver (VTS `0x0833`,
`fps_x100=1501`, 77/0 register mismatches, column FPN unchanged). The CSI-2 receiver's
hsfrequency bin 20 (`{300, 0x14}`) is configuration computed from the declared
`VIDEO_CID_PIXEL_RATE` via `video_get_csi_link_freq()`, not evidence; the evidence is that runs
61/62/63 streamed with zero CSI fatals (a PLL/bin mismatch shows up as `ERRSOTSYNCHS` or
`PHY_FATAL`).

**PLL**: `OV5647_PLL_MULT` is 70 (`0x46`, matching the reference's 640x480 10bpp table): VCO
25 MHz / 3 * 70 = 583.33 MHz, lane bit rate 291.67 Mbps/lane, `OV5647_PIXEL_RATE` 58333333. The
reference declares `.pixel_rate = 55000000` for this mode; `OV5647_PIXEL_RATE` is instead derived
from this driver's PLL constants and bench-matched: run 63 measured `fps_x100=1501` at VTS
`0x0833`/HTS 1852, which the driver's VTS formula gives from 58333333, not 55000000. It is one
global PLL for both the 640x480 binned mode and the crop path, so the crop path also runs at
291.67 Mbps/lane -- safe (a lower bit rate cannot exceed the receiver's timing budget) but
unverified against mainline's crop-mode PLL constants; only 640x480 has bench evidence.

**HTS is per mode**: the 640x480 binned mode writes the reference's `0x380c`/`0x380d` = `0x073c`
(1852); the crop path keeps the driver's bench-proven `2700`. `ov5647_frmrate_to_vts()`,
`ov5647_enum_frmival()` and `ov5647_set_frmival()` derive HTS from the active mode via
`ov5647_hts_for()`.

**Default format and frame rate.** The default format before any `set_format()` call is 640x480
SBGGR10P (`ov5647_init()`'s boot format), where 15 fps is reachable (VTS 2099/`0x0833` >=
`480 + 24`); the non-negotiated camera backend path allocates buffers from this default. A full
resolution crop gets a valid, lower rate -- HTS 2700 cannot sustain 15 fps at 1944 lines, a real
line-time limit -- and `video_closest_frmival()` falls back to the next reachable entry.

**Sticky frame interval.** `ov5647_set_frmival()` stores the caller's whole request in
`data->requested_frmival` (a `struct video_frmival`, so a numerator != 1 such as `{2, 60}`, the
shape Zephyr's `video frmival` shell command sends, is preserved) only after
`video_closest_frmival()` and the VTS write both succeed; `ov5647_set_fmt()` re-requests it on every
format change, so a clamped 10 fps at 2592x1944 does not stick after a later `set_format(640x480)`.
`set_frmival()` reports `{1, ov5647_framerates[fie.index]}` -- what `ov5647_enum_frmival()`
produces for that index -- so it never echoes a request that `video_closest_frmival()` did not
match and that was not applied.

**Flip controls survive a format change.** `ov5647_set_mode_regs()` writes `0x3820`/`0x3821` as
whole bytes, so `ov5647_set_fmt()` re-applies both ctrls (`ov5647_set_ctrl_hflip()` /
`ov5647_set_ctrl_vflip()`, also used by `ov5647_set_ctrl()`) right after it. At the defaults the
mode bytes are unaffected: `TC_REG20_BINNED`/`TC_REG21_BINNED` (`0x41`/`0x01`) already equal the
maintainer-confirmed orientation with bits[2:1] clear.

**Bayer order follows the flip ctrls.** Flipping the sensor flips the Bayer colour order. In terms
of this driver's ctrl values (its `HFLIP` bit is the `0x3821` mirror bit directly, the logical
inverse of the reference's `V4L2_CID_HFLIP`), `(hflip, vflip)` maps `(0,0)` to SBGGR (the
maintainer-confirmed default), `(1,0)` to SGBRG, `(0,1)` to SGRBG and `(1,1)` to SRGGB.
`get_format()` reports the flip-adjusted fourcc and `set_format()` accepts it back (mapping it to
its base fourcc when it equals what the current flip ctrls derive), so a `get_format()` ->
`set_format()` round trip works, as does submitting a base fourcc while flipped. Every failure path
of `set_fmt()` restores `fmt->pixelformat` through a single `err:` label. `get_caps()` is
unchanged: callers request the base SBGGR8/SBGGR10P size/depth. Only the default order is
bench/maintainer-verified; the three flipped orders derive from the reference mapping.

**Orientation (maintainer-confirmed, run 62)**: `0x3821` bits[2:1] are this driver's
`OV5647_TC_REG21_MIRROR` mask. `0x01` (bits clear) is correct and unmirrored; Alif's `0x07` produced
a left-right-mirrored scene. The reference's own `hflip` ctrl sense is inverted relative to this:
it reports `hflip=1` for the register state that is the correct unmirrored orientation on this
silicon. `0x3821 = 0x01` pairs with `MEDIA_BUS_FMT_SBGGR10_1X10`, matching the advertised
SBGGR10P/SBGGR8 formats.

`ov5647_set_mode_regs()` picks the full-FOV binned register set for exactly 640x480 (window, output
size, subsample `0x3814`/`0x3815` = `0x35`, binning `0x3820`/`0x3821` = `0x41`/`0x01`, binned-mode
analog `0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x59`/`0x00`/`0x64`/`0x52`, HTS `0x073c`, and the
50/60 Hz AEC band step `0x3a08`/`0x3a09`/`0x3a0a`/`0x3a0b`/`0x3a0d`/`0x3a0e` and `0x4004`). The band
step is in LINES, so it depends on line time/HTS and lives in the per-mode set, not the common
init. Every other size keeps the centred crop and explicitly re-asserts the 1:1 values
(`0x3814`/`0x3815` = `0x11`, `0x3820`/`0x3821` = `0x40`/`0x00`, HTS `2700`, mainline's
full-resolution analog values `0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x5b`/`0x04`/`0x64`/`0x12`,
cited from mainline and bench-unverified on this module) and the crop path's own AEC band step.
Mainline's band step assumes its 32.51 us line time (HTS 2844 / pixel_rate 87500000) while the crop
path runs a 46.29 us line (`OV5647_HTS_CROP` 2700 / `OV5647_PIXEL_RATE` 58333333), so it is scaled by
the line-time ratio: `0x3a08`/`0x3a09` = `0x00`/`0xd0` (208 lines), `0x3a0a`/`0x3a0b` =
`0x00`/`0xad` (173 lines). The max-bands-per-frame registers `0x3a0d`/`0x3a0e` are computed per crop
size from that height's own minimum-blanking VTS (`floor(VTS/band)`, floored at 1 band); at 2592x1944
(VTS 1968) that is `0x0b`/`0x09`. `0x4004` = `0x04`. All crop-path values are bench-unverified.
Writing the binning registers and then letting `ov5647_set_window()` rewrite the window to a crop
made the sensor emit short lines against a 640-pixel frame declaration and the CSI host raised
"Fatal Interrupt due to mismatch of Frame Start and Frame End" on VC0 44 times in 2 s (bench run
54), so the window, output size, subsample, binning, HTS and AEC-band-step registers are always
written as one coherent set per mode.

**Common init** is sourced from the reference's `ov5647_common_regs[]`, minus `0x0100`/`0x0103`
(park/reset), minus `0x3017` (kept at this driver's `0xf0`, AUTHORIZED LOCAL DIVERGENCE #1/#2),
minus `0x3503` (left to this driver's exposure-control logic), and minus the mode-specific AEC band
step registers (including `0x3a08`). It includes the reference's ISP block enables `0x5000 = 0x06`,
`0x5003 = 0x08`, `0x5a00 = 0x08`, which run 61 streamed clean with. Alif's different ISP set
additionally writes `0x5001`/`0x5002` and `0x4050`/`0x4051` (CSI "incorrect frame sequence" fatals
were seen with that set); none of those four are in the reference table and none are written here.
`0x3000`/`0x3001`/`0x3002` = `0x00` and `0x3018` (MIPI PHY/route control) = `0x44` are literal
reference bytes, superseding the targeted `PHY_PD_MIPI`/`PHY_PD_LPRX`/`MIPI_EN` bitfield modify.

`OV5647_EXPOSURE_DEFAULT` moves from `0x20` (2 lines -- effectively a closed shutter for a
manual-exposure user) to `0x0FFF` (~256 lines), matching the order of magnitude of Alif's shipped
default; the AEC gain/limit registers are unchanged. The `VIDEO_CID_ANALOGUE_GAIN` default stays
`0`: auto-gain (`VIDEO_CID_AUTOGAIN`) defaults on, so the manual gain only applies once auto-gain is
disabled, the same gating `VIDEO_CID_EXPOSURE_AUTO` applies to `OV5647_EXPOSURE_DEFAULT`.

**Low light**: at the default 15 fps VTS in a dark lab the AEC railed at both limits (502 lines = 2x
the `0x3a0b` band step; gain at the `0x3a19` ceiling) and a real image needed ~16000 lines (0.49 s)
-- a scene limitation, not a driver bug. The OV5647 is a colour Bayer sensor; the driver delivers
RAW10 BGGR and demosaic is a downstream ISP job, so raw frames viewed as grey show the Bayer mosaic
as fine stripes, which is not the column fixed-pattern noise above.

The vendored driver's RETIREMENT note in `zephyr/drivers/video/ov5647.c` requires all three fixes -- lane park, PLL + MIPI-TX
pad-drive init, and this full-FOV/per-mode-HTS/AEC-band-step/common-init/exposure-default/
frame-rate/flip-ctrl change -- to be confirmed present upstream before the vendored copy can be
deleted.

`tests/zephyr/video_sensors/src/ov5647_test.c` covers: the full 640x480 binned register block
landing after the `0x3034` bit-mode write and before the lane park; the 640x480 default VTS being
the 15 fps value; `ov5647_enum_frmival()` accepting 60 fps and rejecting 90 fps at 640x480; the
run-54 ordering trap (640x480 -> another size -> back never leaves binning, its HTS or its AEC band
step armed on a crop window, checked via the write log on the crop side too); the frame rate not
sticking at a prior mode's clamped value, and preserving a numerator != 1 request across a format
change; `VIDEO_CID_HFLIP`/`VIDEO_CID_VFLIP` surviving a format change (and producing exactly
`0x41`/`0x01` at their defaults); `get_format()` reporting each of the four Bayer orders, RAW8 and
RAW10 `get_format()` -> `set_format()` round trips, and `fmt->pixelformat` restoration on
unsupported fourcc/size requests (including a flip-shifted request); an exhaustive table-driven
check of every `ov5647_init_regs[]` common-init entry (including `0x303c`/`0x3106`/`0x301c`,
`0x3016`/`0x301d`/`0x3017` and `0x3503`/`0x350c`/`0x350d`); and a guard that `0x5001`, `0x5002`,
`0x4050` and `0x4051` are never written. A `ZTEST_SUITE` before-hook resets both flip ctrls, the
frame interval and the format ahead of every test; the emulator's write-log capacity
(`OV5647_EMUL_LOG_CAPACITY`) is 1024. `docs/camera-shields.md`'s OV5647 row and driver section, and
`docs/adr/0017-alp-sdk-over-the-vendor-sdk.md`, are updated to match.
