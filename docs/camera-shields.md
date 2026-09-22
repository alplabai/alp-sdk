# Raspberry-Pi-style CSI-2 camera shields

## Supported camera modules

| Module | Sensor | Shield | Interface / lanes | Modes | Status |
|---|---|---|---|---|---|
| InnoMaker CAM-OV9281 | OV9281 (1 Mpx global-shutter mono) | `innomaker_cam_ov9281` | MIPI CSI-2 D-PHY, 2 lanes | 640x400 GREY8 @100 fps; 1280x720 GREY8 @50 fps; 1280x800 GREY8 @~100 fps | **Bench-verified** on an E1M-AEN803 on the E1M-EVK (J5), 2026-09-21: all three modes stream live frames, each at its configured rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps); the sensor test pattern is verified in all three modes. |
| RPi Camera Module 1 | OV5647 (5 Mpx raw Bayer) | `raspberry_pi_camera_module_1` | MIPI CSI-2 D-PHY, 2 lanes | up to 2592x1944 SBGGR8/SBGGR10P; 640x480 is a full-array subsampled+binned mode at 15 fps by default, not a crop | **Bench-verified (run 52, RAW10 640x480)** on an E1M-AEN803 on the E1M-EVK (J5), 2026-09-22: `PHY_FATAL` 7712 -> 0, `capture ALP_OK`, 60 frames at 15.96 fps (see the driver section below). Needed the J5 pin-11 pull-up rework -- [`docs/boards/e1m-evk.md`](boards/e1m-evk.md) -- to answer on I2C at all, and a PLL + MIPI-TX pad-drive divergence in the driver the earlier BLOCKED finding misdiagnosed as a module hardware fault. **Run 61 (2026-09-22, current)** replaced runs 56/58/60's Alif-derived 640x480/common-init mix with register values taken as hardware facts from the RPi/OmniVision reference driver (raspberrypi/linux branch rpi-6.6.y, `drivers/media/i2c/ov5647.c`): column fixed-pattern noise fell from 6-8 LSB (3.7-5.9% of full scale) with the runs 56/58/60 mix to 1.0-1.5 LSB (~1.1%) with the reference set, in the same dark lab at the same 0.51 s x15.5 gain exposure -- the visible vertical stripes runs 56/58/60 left in are gone; which specific register difference caused that is NOT ESTABLISHED (see the driver section). **Run 62 bench-verified the COMMITTED driver itself** (77/77 registers matched, zero CSI errors) and the maintainer confirmed a host-demosaiced render of the captured frame is the correct image (BGGR, unmirrored). **Run 62 also fixed a real bug**: the default frame rate at 640x480 had silently been 10 fps, not the intended 15, since `ov5647_init()`'s original boot format made 15 fps unreachable at boot and that low rate stuck for every later open. RAW10 only; RAW8 (SBGGR8) is unverified. |
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

**Bench-verified, run 52 (2026-09-22, an E1M-AEN803 serial 2026W36-0001 on
an E1M-EVK hw_rev 2626-r2, RAW10 640x480).** `PHY_FATAL` went 7712 -> 0,
`alp_camera_open` returned `ALP_OK`, and 60 frames captured at 15.96 fps
(3758 ms for 60 frames), `CAM_FRAME_ADDR` advancing `0x02000080` ->
`0x020960c0`, frame buffer md5 `61cdf7a021584f4bfaa2ae282c7b2256` with
every pre-fill byte overwritten (`a5=0/614400`). The InnoMaker CAM-OV5647
module still needs the pull-up rework described in
[`docs/boards/e1m-evk.md`](boards/e1m-evk.md) before it answers on I2C at
all; that part of the earlier BLOCKED write-up was correct and unchanged.

An earlier bench run (2026-09-22, before run 52) reached only
`alp_camera_open` failing at `ALP_ERR_TIMEOUT` with `E: D-PHY not locked
to Stop-state. PHY status - 0x00010000 DPHY ID: 0`. That `PHY status`
field is `CSI_PHY_RX` (`0x49033048`), the register the driver's log line
prints; `CSI_PHY_STOPSTATE` (`0x4903304c`) reached only `0x00000001`
(bit0 `STOPSTATEDATA_0`), with bit1 (`DATA_1`) and bit16 (`CLK`) never
asserting. **Neither register discriminates a broken link from a
working one on its own**: `CSI_PHY_RX` and `CSI_PHY_STOPSTATE` both read
byte-identically at every capture point in the failing run and in the
working run 52 -- do not use either as a bring-up gate by itself.

**Root cause, corrected: the driver programs NO PLL and no MIPI-TX
pad-drive registers, not a hardware fault in the module.** This
retracts the "fault in the module under test" conclusion previously
published here and in issue #2248 -- the module is healthy. With no PLL
write, the sensor free-ran on its OV5647 power-on defaults: `0x3035` =
`0x11` is PLL system divider 1, giving VCO 875 MHz / sysdiv 1 = 875
Mbps/lane and a 175 MHz pixel clock, while the receiver had been binned
`{450 MHz, 0x16}` from the driver's own declared `pixel_rate` of
83333333 (416.67 Mbps) -- a 2.1x mismatch, producing `ERRSOTSYNCHS` on
both data lanes on every burst while the clock lane still locked (a
clock lane locks far outside the range a data lane can sync SoT in).
Separately, `0x3017` (LP TX pad drive) was also left at its power-on
default, too low to bring `CLOCK` and `DATA_1` to Stop state on this
receiver -- bench-bisected on the same module: reset `0x10` -> no lane
reaches Stop state; `pgm_lptx = 01` -> `DATA_0` only; `10` -> `+DATA_1`;
`11` -> `+CLK`, reaching `CSI_PHY_STOPSTATE` = `0x00010003` (all three
lanes) with `0x3017` = `0xf0`. That bisection is what the earlier write-up
above measured as "`DATA_0` reads 50/50 while `DATA_1` and `CLK` read
0/50" and misread as a module fault: it was the pre-fix driver's `0x3017`
power-on value, not the silicon.

Fixing this in-tree is an AUTHORIZED divergence from the verbatim-backport
rule in `zephyr/drivers/video/ov5647.c`'s header (a second one, alongside
the earlier lane-park fix) -- which is why that header now carries a
retirement warning covering BOTH fixes: when the Zephyr pin advances to a
revision containing zephyrproject-rtos/zephyr#119301, confirm both are
present upstream BEFORE deleting the vendored copy, or the deletion
silently reintroduces one or both bugs.

1. **FIXED (issue #2248) -- the driver never performed mainline's LP-11
   park.** Mainline's `ov5647_power_on()` calls `ov5647_stream_stop()`
   under the comment "Stream off to coax lanes into LP-11 state"; the
   vendored driver only ever wrote `0x0100`. In software standby this
   part presents no LP-11 at all (`CSI_PHY_STOPSTATE` = `0x00000000`), so
   on a receiver that gates on Stop-state before stream start -- as this
   one does -- even a healthy OV5647 could not open. `ov5647_init()` now
   leaves the sensor parked and `set_stream()` unparks and re-parks
   around streaming, with `0x0100` = `0x01` written FIRST. `ov5647_set_fmt()`
   also drops to standby before its register writes and re-parks
   afterwards, and `ov5647_lane_park()`'s error paths make a best-effort
   drop back to standby rather than leaving the sensor streaming
   mid-sequence.
2. **FIXED (issue #2248, bench run 52) -- the PLL and MIPI-TX pad-drive
   registers were never programmed.** Eight registers (`0x3017 0x3035
   0x3036 0x3037 0x303c 0x301c 0x301d 0x3106`) are now written in
   `ov5647_init_regs[]`, in software standby immediately after the
   `0x0103` software reset and before the lane park -- the PLL dividers
   latch at that reset, not on standby exit, so the same writes issued
   after the park update the register file without moving the running
   PLL (a bench run 51 mistake). `0x3017` = `0xf0`
   (`pgm_lptx` = 3, max) is a deliberate divergence from mainline's
   `0xe0` (`pgm_lptx` = 2): mainline's value never brings the clock lane
   to Stop state, which mainline's own receiver does not gate on but ours
   does. `0xf0` is pinned at maximum drive strength with **no
   characterised margin** against overdrive -- narrowing it needs bench
   time this run did not spend. Which of the eight registers are
   individually load-bearing is **not established**; only `0x3017` is
   independently bisected above. `0x4837` (PCLK period) stays at its
   power-on `0x15` (mainline writes `0x19` for this PLL); on the safe
   side at whatever bit rate the currently-programmed PLL gives (see
   item 4 below for the CURRENT value -- this item only fixed that a
   PLL was written at all, not which one).
   **AT THE TIME (run 52), the specific values matched mainline's
   full-resolution constants**: `0x3037` = `0x03` (prediv 3), `0x3036` =
   `0x69` (multiplier 105), `0x3035` = `0x21` (sysdiv 2) -> VCO 875 MHz /
   sysdiv 2 = 437.5 Mbps/lane, matching mainline's declared `link_freq`
   218750000 (DDR half-rate) for the full-resolution mode. **SUPERSEDED
   by run 61** (item 4 below): the multiplier is now 70 (`0x3036` =
   `0x46`), matched to mainline's 640x480 10bpp table instead, since
   640x480 is the mode this driver actually ships -- do not read `0x69`
   or 437.5 Mbps as the current value; see item 4 for the current PLL.
3. **`OV5647_PIXEL_RATE` corrected, not "disproven".** A previous note
   here said the claim that `PIXEL_RATE = XVCLK * 10 / 3` = 83333333
   understated the link rate was investigated and DISPROVEN, and that a
   follow-up correction to a PLL-derived 175000000 was reverted for
   doubling the programmed frame length. That conclusion is now itself
   retracted: the 175000000 figure was actually right about the
   power-on link rate, and "it doubled the programmed VTS" was the
   correct *consequence* of a doubled pixel clock, not evidence against
   it. The real defect was that the PLL was never WRITTEN to match
   either number. `OV5647_PIXEL_RATE` derives from the same
   `OV5647_PLL_PREDIV` / `OV5647_PLL_MULT` / `OV5647_PLL_SYS_DIV`
   constants item 2 programs into the sensor -- **AT THE TIME (run 52)
   this gave 87500000 at 25 MHz XVCLK, matching mainline's full-
   resolution `pixel_rate`; SUPERSEDED by run 61's PLL correction (item
   4), which gives the CURRENT value 58333333.** `TIMING_VTS` is now
   derived per the ACTIVE mode's HTS, not one constant -- see item 4;
   `ov5647_enum_frmival()` still behaves sanely, just narrowing which of
   the fixed frame rates are reachable at a given height and HTS.
   **8-bit is still not bench-verified**: this derivation is fixed at
   10bpp (the only bench-proven bpp); `cfg->pixel_rate` is a single
   DT-derived constant, not re-derived per selected format, so selecting
   `VIDEO_PIX_FMT_SBGGR8` (8bpp) still reports the RAW10 value against
   `video_get_csi_link_freq()`'s fallback, which would need 72916666
   (291.67 Mbps * 2 / 8, the CURRENT PLL) at the same PLL to be correct
   for that format -- flagged, not fixed, pending a bench measurement of
   the 8-bit path.
4. **FIXED (issue #2248, bench runs 56/58/60/61/62 -- run 61 is
   current, source of truth) -- 640x480 was a heavy telephoto crop, not
   the sensor's real 640x480 mode, and the driver never wrote the
   common analog/BLC/AEC registers the reference driver writes for
   every mode.** The driver's 640x480 was a 648x488 1:1 centre crop --
   roughly 25% of the array width -- not the full-array
   subsampled+binned 640x480 mode. The register VALUES are taken as
   hardware facts from the RPi/OmniVision reference driver:
   raspberrypi/linux branch rpi-6.6.y, `drivers/media/i2c/ov5647.c`,
   `ov5647_common_regs[]` and `ov5647_640x480_10bpp[]` (GPL-2.0 -- this
   file copies no source text from that driver, only numeric register
   addresses/values). Run 61 ran those tables closely but NOT
   byte-identically: `0x0100`/`0x0103` are owned by this driver's own
   reset/park sequence, not the table; the table's `0x3017` = `0xe0`
   was written but this driver's re-park step then overwrote it to OUR
   `0xf0` before streaming (see items 1/2 above), so the stream always
   started with `0xf0`; `0x3503` was left to the app's own exposure
   control rather than the table's `0x03`; and `0x4800` used this
   driver's park/stream-on sequence value (`0x04`), not the table's
   `0x34`. Run 61 streamed clean: "[stg] 75 regs 0 failed", MFR start
   `ALP_OK`, zero `E:` lines / CSI fatals, frame CRC-matched against a
   real scene, at the CSI-2 receiver's matched hsfrequency bin 20
   (`{300, 0x14}`) for the corrected PLL -- that bin match, not any
   receiver-side clock readback, is the PLL-lock evidence; a receiver
   register that merely echoes our own declared `VIDEO_CID_PIXEL_RATE`
   back through `video_get_csi_link_freq()` is not independent
   confirmation.
   **Run 62 bench-verified the COMMITTED driver itself**, not a
   modified bench app: the test app wrote no mode registers and read
   back 77 registers against the reference, 0 mismatches; it streamed
   with zero `E:` lines; column fixed-pattern noise matched run 61
   plane by plane (1.65 / 1.19 / 1.18 / 0.97 LSB, ~1.1% of signal). The
   maintainer then viewed a host-demosaiced colour render of the run-62
   frame and **CONFIRMED it is the correct image**: orientation
   `0x3821 = 0x01` / `0x3820 = 0x41` (see below) is unmirrored and
   correct, Bayer order BGGR, green channels diagonal and equal
   (107.2 / 106.8) -- maintainer-confirmed on run 62's actual rendered
   frame, not inferred from register semantics alone.
   Alif's own OV5647 table for this exact silicon (Alif Ensemble
   CMSIS-DFP package "AlifSemiconductor.Ensemble",
   `components/Source/OV5647_camera_sensor.c:118-136`, package v2.1.0)
   is a **variant** of the same reference, not an independent source:
   it shares most analog/BLC/AEC values with the reference but diverges
   at `0x3821` (Alif's `0x07` vs the reference's `0x01` -- bits[2:1] of
   `0x3821` are this driver's horizontal-mirror mask, NOT "analog
   timing") and writes a DIFFERENT ISP-enable set (`0x5000=0x06,
   0x5001=0x01, 0x5002=0x41, 0x5003=0x08, 0x5a00=0x08` -- the reference
   and this driver write only `0x5000`/`0x5003`/`0x5a00`).
   **WHICH of runs 56/58/60's several changes vs runs 61/62 (PLL, HTS,
   `0x3821`, the ISP-enable set, and `0x3000..0x3002`) actually caused
   the visible vertical stripes runs 56/58/60 bench-proved as "clean"
   is NOT ESTABLISHED** -- run 61 changed all of them together against
   the reference, not one at a time. (Runs 56/58/60 ran Alif's
   `0x3821 = 0x07` on `OV5647_PLL_MULT = 105`, the SAME PLL run 52
   originally fixed -- not run 61's corrected 70 -- so the stripes
   cannot be attributed to a PLL/`0x3821` interaction specifically
   without more bisection than this investigation did.) What runs
   61/62 DO establish is the outcome: the reference register set,
   applied together, streams clean with the column-FPN numbers below;
   that is the basis for shipping it, not a claim about which single
   register mattered most.
   Run 61's 640x480 register set: full-array window (unchanged from
   runs 56/60), output size 640x480, subsample `0x3814`/`0x3815` =
   `0x35`, binning-enable `0x3820` = `0x41` with `0x3821` = `0x01` (NOT
   runs 56/58/60's `0x07`), binned-mode analog
   `0x3612`/`0x3618`/`0x3708`/`0x3709` = `0x59`/`0x00`/`0x64`/`0x52`
   (unchanged), the 50/60 Hz AEC band step `0x3a09`/`0x3a0a`/`0x3a0b`/
   `0x3a0d`/`0x3a0e` and `0x4004` (issue #2248 fix-up: moved out of the
   common init into this per-mode block -- the band step is in LINES,
   which depends on line time/HTS, so a value correct for this mode's
   HTS is wrong for the crop path's), and a **per-mode line length**,
   `0x380c`/`0x380d` = `0x073c` (1852, the reference value) -- the crop
   path keeps the driver's own bench-proven `2700` (run 52) instead of
   mainline's full-resolution `2844`, which is unverified here.
   `ov5647_set_mode_regs()` picks this full-FOV binned set for exactly
   640x480 and keeps the existing centred crop for every other size --
   but the crop path now also explicitly re-asserts the 1:1
   subsample/binning/analog/HTS values (`0x3814`/`0x3815` = `0x11`,
   `0x3820`/`0x3821` = `0x40`/`0x00`, `2700`, mainline's
   full-resolution analog values `0x3612`/`0x3618`/`0x3708`/`0x3709` =
   `0x5b`/`0x04`/`0x64`/`0x12`, cited from mainline and
   BENCH-UNVERIFIED on this module, and the SAME AEC band-step values
   as the binned mode -- no mainline full-resolution citation is
   available, so they are reused rather than left unwritten, which
   would silently retain a prior mode's stale line counts;
   BENCH-UNVERIFIED at this HTS), so a prior 640x480 selection can
   never leave binning (or its HTS/band step) armed on a later crop
   window.
   **Orientation (maintainer-confirmed, run 62):** `0x3821` bits[2:1]
   are this driver's existing horizontal-mirror mask; `0x01`
   (reference) vs `0x07` (Alif's variant) produced a LEFT-RIGHT-MIRRORED
   scene on the bench at `0x07` -- `0x01` is correct and unmirrored, as
   confirmed by the maintainer's review of a host-demosaiced run-62
   frame (BGGR, greens diagonal and equal). Per the RPi driver,
   `0x3821 = 0x01` with `0x3820 = 0x41` is its hflip=1/vflip=0
   configuration, paired with `MEDIA_BUS_FMT_SBGGR10_1X10` -- matching
   this driver's advertised SBGGR formats.
   **Ordering trap (bench run 54, unchanged):** binning is only
   coherent with the full-array window -- run 54 applied the binning
   registers and then let `ov5647_set_window()` rewrite the window to
   the crop, and the sensor emitted short lines against a 640-pixel
   frame declaration; the CSI host raised "Fatal Interrupt due to
   mismatch of Frame Start and Frame End" on VC0 44 times in 2 s and
   delivered nothing. The window, output size, subsample, binning,
   line-length AND AEC-band-step registers are always written as one
   coherent set per mode.
   **Common init (run 61, verified as shipped by run 62):** register
   VALUES match runs 56/58/60's Alif-derived block for most addresses,
   but the SOURCE is now the reference's `ov5647_common_regs[]`, minus
   `0x0100`/`0x0103` (park/reset), minus `0x3017` (kept at OUR `0xf0`,
   see items 1/2 above), minus `0x3503` (left to this driver's own
   exposure-control logic), and minus the mode-specific AEC band-step
   registers (moved to the per-mode block above). UNLIKE runs 56/58/60,
   it **includes** the reference's ISP block enables `0x5000 = 0x06`,
   `0x5003 = 0x08`, `0x5a00 = 0x08` -- run 61 streamed clean with them.
   Run 58's CSI "incorrect frame sequence" fatals, previously blamed on
   ISP enables as a category, actually came from Alif's DIFFERENT ISP
   set specifically, which additionally writes `0x5001`/`0x5002` and
   (in runs 56/58/60's now-superseded BLC section) `0x4050`/`0x4051` --
   none of those four are in the reference table and none are written
   here; do not add them back.
   `OV5647_EXPOSURE_DEFAULT` (unchanged by run 61) still moves from
   `0x20` (2 lines -- effectively a closed shutter for a
   manual-exposure user) to `0x0FFF` (~256 lines), matching the order
   of magnitude of Alif's own shipped default (~`0x000FFF`) for this
   silicon; the AEC gain/limit registers themselves are left unchanged.
   **Low light**: at the default 15 fps VTS in a dark lab the AEC
   railed at both its limits (502 lines, gain at ceiling) and a real
   image needed ~16000 lines (0.49 s) -- a scene limitation, not a
   driver bug.
5. **FIXED (issue #2248, bench run 62) -- the default frame rate at the
   shipped 640x480 mode was silently 10 fps, not 15.** `ov5647_init()`
   used to boot into the full-resolution crop (`OV5647_FULL_WIDTH`/
   `HEIGHT`); at that height and the crop-path HTS (2700), the driver's
   own default 15 fps needs VTS 1440, below `1944 + 24 = 1968`, so it
   was unreachable. Zephyr's `video_closest_frmival()` stops
   enumerating at the first unreachable (sorted-ascending) rate, so it
   never even looked past 10 fps -- and that 10 fps then stuck in
   `data->frmrate` for every LATER `ov5647_set_fmt()` call, including
   the 640x480 mode this driver actually ships, since nothing in
   `src/backends/camera` or `aen-camera-firstlight` ever calls
   `set_frmival` to override it. `ov5647_init()` now boots directly
   into 640x480, where 15 fps IS reachable (VTS 2099/`0x0833` >=
   `480 + 24`), so the shipped default is correctly 15. A caller that
   explicitly switches to a full-resolution crop still gets a valid,
   if lower, rate -- HTS 2700 genuinely cannot sustain 15 fps at 1944
   lines; that is a real PLL/line-time limit, not a bug.

**Raw-frame stripes are not a bug.** What looks like banding/noise in a
raw capture viewed as greyscale is (a) the Bayer colour-filter mosaic
itself (the OV5647 is a colour Bayer sensor; the driver delivers RAW10
BGGR, and demosaic/colour reconstruction is a downstream ISP job -- see
run 62's maintainer-confirmed colour render above), and (b) column
fixed-pattern noise that scales with analog gain. With the run-61/62
reference register set this measures roughly 1.0-1.5 LSB standard
deviation (~1.1% of full scale) at x15.5 gain in a dark lab at 0.51 s
exposure -- down from 6-8 LSB (3.7-5.9%) with runs 56/58/60's
Alif-derived mix under the same conditions. WHICH specific register
difference (of several changed together in run 61) caused that
reduction is NOT ESTABLISHED -- see item 4 above. Both figures are
normal sensor behaviour in kind -- column fixed-pattern noise that
scales with gain -- and only become visually dominant at high gain in
low light; neither indicates a driver defect, but the magnitude is
register-set-dependent, which is why run 61 replaced runs 56/58/60's
mix outright rather than patching it further.

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
and OV5647 (`ov5647_test.c`), each against its own I2C emulator, on
`native_sim` and `native_sim/native/64`: OV9281 checks
`get_caps`/`set_format`/register programming/exposure range check; OV5647
checks the chip-ID probe, the lane-park sequence and its write ORDER (not
just final register values), the PLL/MIPI-TX pad-drive registers from
issue #2248's run-52 fix landing before the park (not after), the
full-FOV binned 640x480 mode (including its per-mode HTS and reference
orientation, run 61) landing before the park, the common analog/BLC/AEC
init landing before the first running `MODE_SELECT`, and that switching
640x480 -> another size -> back never leaves binning (or its HTS) armed
on a crop window (the run-54 ordering trap). IMX296 stays
compile-coverage in that same runtime suite (no emulator -- see
`app.overlay`), mirroring upstream's `tests/drivers/build_all/video` shape
for the compile-only case.

## First-light example

`examples/aen/aen-camera-firstlight` opens each shield through the portable
`<alp/camera.h>` API (the upstream `raspberry_pi_camera_module_2`/IMX219
shield also builds against it) and captures one frame with a timeout, on the
E1M-EVK's `e1m_evk_rpi_csi` carrier connector shield. See that example's
README for what each printed line means and the expected result per module
-- the OV9281 path is bench-verified (2026-09-21) and the OV5647 path is
now also bench-verified, RAW10 only (run 52, 2026-09-22), both on an
E1M-AEN803 on the E1M-EVK; see the OV5647 driver section above for the
root-cause writeup and its honest limits. The IMX219 and IMX296 paths
compile and link against the real board target but have not yet been run
on real silicon.
