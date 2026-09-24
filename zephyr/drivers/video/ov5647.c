/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1 (upstream-PENDING backport, INTERIM) ======
 * Backport of the OmniVision OV5647 Zephyr video-sensor driver from the
 * still-open upstream PR zephyrproject-rtos/zephyr#119301 ("drivers: video:
 * add a driver for the OmniVision OV5647", author kartben) @ commit
 * d81b1f630ee0f5078de6e3a47e7bc1b3613424c2. Not yet merged upstream and not
 * present in the pinned Zephyr v4.4.1 base, so it is carried here verbatim
 * (Apache-2.0) to unblock the Raspberry Pi Camera Module 1 shield.
 *
 * Kept byte-for-byte apart from what alp-sdk's out-of-tree module build
 * requires (this file header, and three extra includes
 * (<zephyr/drivers/video-controls.h>, "video_ctrls.h", "video_device.h")
 * the PR's target tree apparently pulls in transitively through
 * "video_common.h" but the pinned Zephyr v4.4.1 here does not --
 * struct video_ctrl_range / video_ctrl / VIDEO_DEVICE_DEFINE live in
 * those headers on this pin, see zephyr/drivers/video/imx219.c, which
 * includes all three explicitly) and apart from the three AUTHORIZED LOCAL
 * DIVERGENCES below. See zephyr/CMakeLists.txt / zephyr/kconfigs/ for the
 * build hookup, mirroring how arx3a0.c is wired. The upstream
 * symbol VIDEO_OV5647 and compatible "ovti,ov5647" are used as-is (not
 * renamed) so the eventual retirement is a clean deletion, not a rename --
 * verified against upstream's own drivers/video/Kconfig namespace (no other
 * in-flight Zephyr driver claims VIDEO_OV5647), so a same-named symbol
 * landing on the version bump cannot silently collide with a DIFFERENT
 * driver; it can only be the merged form of this same PR.
 *
 * AUTHORIZED LOCAL DIVERGENCE #1 (issue #2248): the driver never drove the
 * CSI-2 lanes to LP-11 (Stop state), so a receiver that waits for Stop-state
 * before stream start (e.g. a DesignWare CSI-2 host) could not open it.
 * Bench evidence on an E1M-AEN803 / E1M-EVK (hw_rev 2626-r2):
 * CSI_PHY_STOPSTATE read 0x00000000 in bare software standby -- mirrors
 * mainline Linux's ov5647_power_on()/ov5647_stream_stop() "coax lanes into
 * LP-11" step. Fixed by ov5647_lane_park() below. An earlier version of
 * this note also claimed the park alone took CSI_PHY_STOPSTATE only to
 * 0x00000001 because of "a hardware fault on the module under test". That
 * claim is RETRACTED -- see DIVERGENCE #2 below for the real cause.
 *
 * AUTHORIZED LOCAL DIVERGENCE #2 (issue #2248, bench run 52): the driver
 * programs NO PLL and no MIPI-TX pad-drive registers, so it free-runs on
 * OV5647 power-on defaults -- 0x3035 = 0x11 is PLL system divider 1, giving
 * 875 Mbps/lane and a 175 MHz pixel clock, while the receiver was binned
 * {450 MHz, 0x16} from this driver's own declared pixel_rate of 83333333
 * (416.67 Mbps): a 2.1x mismatch, giving ERRSOTSYNCHS on both data lanes on
 * every burst, with the clock lane locking regardless (a clock lane locks
 * far outside the range a data lane can sync SoT in). Power-on defaults
 * also left 0x3017 (LP TX pad drive) too low to bring CLOCK and DATA_1 to
 * Stop state, which is what DIVERGENCE #1's "hardware fault" claim actually
 * measured: bench-bisected on the same module, reset 0x10 -> no lane
 * reaches Stop state; pgm_lptx = 01 -> DATA_0 only; 10 -> +DATA_1; 11 ->
 * +CLK. CSI_PHY_STOPSTATE reads 0x00010003 (all three lanes) with 0x3017 =
 * 0xf0. Fixed by the eight-register write in ov5647_init_regs[] documented
 * at its own block comment below, with PLL values matched to mainline
 * Linux's own declared constants for this mode (see the
 * OV5647_PLL_PREDIV/MULT/SYS_DIV comment near OV5647_PIXEL_RATE). Bench
 * evidence (run 52, E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev
 * 2626-r2, RAW10 640x480): PHY_FATAL 7712 -> 0, capture ALP_OK, 60 frames
 * at 15.96 fps, frame buffer md5 61cdf7a021584f4bfaa2ae282c7b2256 with
 * every pre-fill byte overwritten. Mainline writes 0x3017 = 0xe0 (pgm_lptx
 * = 10, one step lower); that never brings the CLOCK lane to Stop state,
 * which mainline's own receiver does not gate on but ours does -- our 0xf0
 * is a deliberate divergence from mainline, pinned at maximum drive
 * strength with NO characterised margin against overdrive.
 *
 * NOT ESTABLISHED: which of the eight registers run 52 wrote are
 * individually load-bearing -- only 0x3017 is independently bisected above.
 * The values matched mainline's full-resolution constants, which were the reference at the time,
 * so shipping them together was right, but do not read this as proof the PLL pair alone is
 * the whole fix. 0x4837 (PCLK period) is left at its power-on 0x15
 * (mainline writes 0x19 for this PLL); on the safe side at run 52's 437.5 Mbps and now safer
 * still at run 61's 291.67 Mbps (AUTHORIZED LOCAL DIVERGENCE #3, see OV5647_PLL_MULT) -- not
 * changed in either working run.
 *
 * AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248, bench runs 56/58/60/61/62): the driver's 640x480
 * was a 648x488 1:1 CENTRE CROP -- roughly 25% of the array width, a heavy telephoto crop -- not
 * the sensor's real full-array subsampled+binned 640x480 mode. The register VALUES this
 * divergence programs are taken as hardware facts from the RPi/OmniVision reference driver:
 * raspberrypi/linux branch rpi-6.6.y, drivers/media/i2c/ov5647.c, ov5647_common_regs[] and
 * ov5647_640x480_10bpp[] (GPL-2.0) -- this file copies no source TEXT from that driver, only the
 * numeric register addresses/values, which are hardware facts about the silicon, not expression.
 * Run 61 ran those tables closely but NOT byte-identically: 0x0100/0x0103 are owned by this
 * driver's own reset/park sequence, not the table; the table's 0x3017 = 0xe0 was written but this
 * driver's re-park step (AUTHORIZED LOCAL DIVERGENCE #1) then overwrote it to OUR 0xf0 before
 * streaming, so the stream always started with 0xf0 (see DIVERGENCE #1/#2 for why); 0x3503 was
 * left to the app's own exposure control rather than the table's 0x03; and 0x4800 used this
 * driver's park/stream-on sequence value (0x04), not the table's 0x34. Run 61 streamed clean:
 * "[stg] 75 regs 0 failed", MFR start ALP_OK, zero E: lines / CSI fatals, frame CRC-matched
 * against a real scene. The CSI-2 receiver's hsfrequency bin 20 ({300, 0x14}) is CONFIGURATION,
 * not evidence: video_csi_dw.c sets phy->pll_fin = link_freq, itself derived from our own
 * declared VIDEO_CID_PIXEL_RATE (58333333) via video_get_csi_link_freq(), and dphy_dw.c picks the
 * bin from that -- so bin 20 follows from what we DECLARED regardless of what the silicon's real
 * PLL is doing; it is not an independent receiver-side measurement and is not cited here as one.
 * The actual evidence is that runs 61/62/63 (below) streamed with ZERO CSI fatals while the
 * receiver was configured for that bin -- a real mismatch would show up as ERRSOTSYNCHS or
 * PHY_FATAL, the same failure signature DIVERGENCE #2 above bisected. Clean streaming at a
 * configuration is the evidence, not the configuration itself.
 *
 * Run 62 BENCH-VERIFIED THE COMMITTED DRIVER (this file, as shipped) on its own, not a modified
 * bench app: the test app wrote no mode registers itself and read back 77 registers against the
 * reference, 0 mismatches; it streamed with zero E: lines; column fixed-pattern noise matched run
 * 61 plane by plane (1.65 / 1.19 / 1.18 / 0.97 LSB, ~1.1% of signal). The maintainer then viewed a
 * host-demosaiced colour render of the run-62 frame and CONFIRMED it is the correct image:
 * orientation 0x3821 = 0x01 / 0x3820 = 0x41 (see below) is unmirrored and correct, Bayer order
 * BGGR, green channels diagonal and equal (107.2 / 106.8). Orientation is MAINTAINER-CONFIRMED on
 * run 62's actual rendered frame, not inferred from datasheet register semantics alone.
 *
 * Alif's own OV5647 table for this exact silicon (Alif Ensemble CMSIS-DFP package
 * "AlifSemiconductor.Ensemble", components/Source/OV5647_camera_sensor.c:118-136, package
 * v2.1.0) is a VARIANT of the same RPi/OmniVision reference, not an independent source: it shares
 * most analog/BLC/AEC values with the reference but diverges at 0x3821 (Alif 0x07 vs the
 * reference's 0x01 -- bits[2:1] of 0x3821 are this driver's OV5647_TC_REG21_MIRROR mask, NOT
 * "analog timing") and writes a DIFFERENT ISP-enable set (0x5000=0x06, 0x5001=0x01, 0x5002=0x41,
 * 0x5003=0x08, 0x5a00=0x08 -- the reference and this driver write only 0x5000/0x5003/0x5a00; see
 * the common-init block comment below for why 0x5001/0x5002 are deliberately excluded here).
 * WHICH of runs 56/58/60's several changes vs runs 61/62 (PLL, HTS, 0x3821, the ISP-enable set,
 * and 0x3000..0x3002) actually caused the visible vertical stripes runs 56/58/60 bench-proved as
 * "clean" is NOT ESTABLISHED -- run 61 changed all of them together against the reference, not
 * one at a time, so no single register is independently bisected as the stripe cause. (Runs
 * 56/58/60 ran Alif's 0x3821 = 0x07 on OV5647_PLL_MULT = 105, the SAME PLL run 52 originally
 * fixed -- not run 61's corrected 70 -- so the stripes cannot be attributed to a PLL/0x3821
 * interaction specifically without more bisection than this investigation did.) What run 61/62
 * DO establish is the outcome: the reference register set, applied together, streams clean with
 * the column-FPN numbers above; that is the basis for shipping it, not a claim about which single
 * register mattered most.
 *
 * 640x480 mode (bench runs 61/62, the reference's ov5647_640x480_10bpp[]): full-array window
 * (unchanged from runs 56/60: 0x3800..0x3807 = 0x0010,0x0000..0x0a2f,0x079f), output size
 * 640x480 (0x3808/0x3809 = 640, 0x380a/0x380b = 480), subsample 0x3814/0x3815 = 0x35,
 * binning-enable 0x3820 = 0x41 with 0x3821 = 0x01 (NOT runs 56/58/60's 0x07 -- see the
 * orientation note above), binned-mode analog 0x3612/0x3618/0x3708/0x3709 =
 * 0x59/0x00/0x64/0x52 (unchanged from runs 56/60), the 50/60 Hz AEC band step
 * 0x3a08/0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e and 0x4004 (issue #2248 fix-up: these MOVED here from
 * ov5647_init_regs[] -- the band step is in LINES, which depends on line time/HTS, so a value
 * correct for this mode's HTS is wrong for the crop path's different HTS; see the
 * ov5647_set_mode_regs() block comment), and a PER-MODE line length, 0x380c/0x380d = 0x073c
 * (1852) -- see OV5647_HTS_640X480_BINNED and ov5647_hts_for() below; the driver-wide
 * OV5647_HTS_CROP (2700, bench-proven, run 52) stays for every other size, since a wide 1:1 crop
 * needs the longer line (mainline's own full-resolution table uses 2844, itself UNVERIFIED
 * here). Written in software standby after ov5647_set_window() and before the lane park, one
 * coherent block per ov5647_set_mode_regs() below.
 *
 * ORDERING TRAP (bench run 54, MUST NOT be reintroduced): binning is only coherent with the
 * full-array window -- 0x3814 = 0x35 is a ~/4 decimation (2592/4 = 648, 1944/4 = 486). Run 54
 * applied the binning registers and then let ov5647_set_window() rewrite the window to the
 * crop; the sensor emitted short lines against a 640-pixel frame declaration and the CSI host
 * raised "Fatal Interrupt due to mismatch of Frame Start and Frame End" on VC0 44 times in 2 s
 * and delivered nothing. So the window, output size, subsample, binning, line-length AND
 * AEC-band-step registers are always written as ONE coherent set per mode -- never binning (or
 * the binned mode's HTS/band step) left on with a crop window. Every OTHER requested size
 * therefore ALSO explicitly writes the 1:1 values (0x3814/0x3815 = 0x11, 0x3820/0x3821 =
 * 0x40/0x00, and OV5647_HTS_CROP) plus mainline's full-resolution 1:1 analog values
 * (0x3612/0x3618/0x3708/0x3709 = 0x5b/0x04/0x64/0x12, cited from mainline, BENCH-UNVERIFIED on
 * this module) and its OWN AEC band-step values (0x3a08/0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e,
 * line-time-scaled from mainline's full-resolution table to this driver's crop-path line time --
 * see the block comment in ov5647_set_mode_regs() for the arithmetic; BENCH-UNVERIFIED, flagged
 * not proven) right after ov5647_set_window(), so switching 640x480 -> another size -> back never
 * leaves binning (or its HTS/band step) armed on a stale crop window.
 *
 * Common init (bench run 61, verified as shipped by run 62): the register VALUES match the
 * reference's ov5647_common_regs[] (same source as above), minus 0x0100/0x0103 (owned by this
 * driver's own park/reset), minus 0x3017 (kept at OUR 0xf0 -- see DIVERGENCE #1/#2 above, a
 * deliberate divergence from both mainline's 0xe0 and the reference driver's table value, which
 * this driver's re-park overwrites before streaming regardless), minus 0x3503 (left to this
 * driver's existing ov5647_set_ctrl_exposure()/OV5647_MANUAL_CTRL logic, not a static table
 * value), and minus the mode-specific AEC band-step registers (moved to ov5647_set_mode_regs(),
 * see above). UNLIKE runs 56/58/60, it INCLUDES the reference's ISP block enables 0x5000 = 0x06,
 * 0x5003 = 0x08, 0x5a00 = 0x08 -- run 61 streamed clean with them. The run-58 CSI "incorrect
 * frame sequence" fatals that runs 56/58/60 blamed on "the ISP block enables" generically came
 * from Alif's DIFFERENT ISP set specifically, which additionally writes 0x5001/0x5002 (and, in
 * runs 56/58/60's now-superseded BLC section, 0x4050/0x4051) -- none of those four are in the
 * reference table and none are written here. Do NOT add 0x5001, 0x5002, 0x4050 or 0x4051.
 *
 * OV5647_EXPOSURE_DEFAULT (unchanged by run 61): was 0x20 (2 lines in 1/16-line units) --
 * effectively a closed shutter for any manual-exposure user. Now 0x0FFF (~256 lines), matching
 * the order of magnitude of Alif's own shipped default (~0x000FFF) for this silicon. At the
 * default 15 fps VTS in a dark lab the AEC railed at both its limits (502 lines, 2x the 0x3a0b
 * band step; gain at the 0x3a19 ceiling) and a real image needed ~16000 lines (0.49 s) -- a
 * SCENE limitation, not a driver bug; the AEC limit registers are left unchanged.
 *
 * FIXED (issue #2248, bench run 62): ov5647_init() booted into the full-resolution crop, which
 * made the driver's default 15 fps unreachable at that height and left every LATER open --
 * including the 640x480 mode this driver actually ships -- silently stuck at 10 fps. See the
 * FIXED comment on ov5647_init()'s own fmt initialiser below for the full mechanism.
 *
 * ACCEPTED COST: ov5647_init() runs at POST_KERNEL on every board that
 * enables this driver and now leaves the sensor parked -- running, with
 * frames suppressed -- for the life of the system rather than in software
 * standby, because the shield deliberately has no pwdn-gpios to put it in a
 * lower-power state instead. Recorded and accepted, not an oversight.
 *
 * RETIREMENT: delete this file + Kconfig.ov5647 + the ovti,ov5647.yaml
 * binding, and drop the CMake/Kconfig hookup, the moment the alp-sdk Zephyr
 * pin advances to a revision that contains #119301 (i.e. the vendored copy
 * and the upstream driver would otherwise both define VIDEO_OV5647 /
 * "ovti,ov5647" and collide) -- BUT NOT BEFORE ALL THREE fixes above are
 * either re-applied to the upstream-derived driver or confirmed already
 * present in it: DIVERGENCE #1 (the LP-11 lane park), DIVERGENCE #2 (the
 * PLL + MIPI-TX pad-drive init, including the corrected OV5647_PIXEL_RATE
 * derivation), AND DIVERGENCE #3 (the RPi/OmniVision-reference full-FOV binned 640x480 mode with
 * its per-mode HTS and AEC band step, the run-61 PLL correction, the matching common init, the
 * corrected OV5647_EXPOSURE_DEFAULT, and the run-62 default-frame-rate fix in ov5647_init()) --
 * deleting this file without checking all three silently
 * reintroduces one or more bugs. Do NOT otherwise maintain divergent local
 * patches on this file -- open a new PR against upstream instead and
 * re-backport. See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ====================================================================
 */

#define DT_DRV_COMPAT ovti_ov5647

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/dt-bindings/video/video-interfaces.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/video/video.h>

#include "video_common.h"
#include "video_ctrls.h"
#include "video_device.h"

LOG_MODULE_REGISTER(video_ov5647, CONFIG_VIDEO_LOG_LEVEL);

#define OV5647_CHIP_ID 0x5647

#define OV5647_FULL_WIDTH  2592
#define OV5647_FULL_HEIGHT 1944

/* Position of the first output pixel in the pixel array, from the power-on window */
#define OV5647_X_ADDR_START 12
#define OV5647_Y_ADDR_START 4

/*
 * The read-out window is wider and taller than the output window by this many pixels, which the
 * ISP crops back off using the offsets at 0x3810..0x3813 left at their power-on values.
 */
#define OV5647_WINDOW_MARGIN 8

/*
 * Line length is now PER MODE (AUTHORIZED LOCAL DIVERGENCE #3, bench run 61) -- see
 * ov5647_hts_for() near ov5647_set_mode_regs() below, which picks between these two and is the
 * only thing that should ever read either constant. OV5647_HTS_CROP (bench-proven, run 52) is
 * the crop path's line length, kept at its original power-on value; OV5647_HTS_640X480_BINNED is
 * the RPi/OmniVision reference value (raspberrypi/linux rpi-6.6.y drivers/media/i2c/ov5647.c,
 * ov5647_640x480_10bpp[]) for the 640x480 binned mode -- a wide 1:1 crop needs the longer line
 * (mainline's own full-resolution table uses 2844, UNVERIFIED here; OV5647_HTS_CROP keeps run
 * 52's bench-proven 2700 instead).
 */
#define OV5647_HTS_CROP           2700
#define OV5647_HTS_640X480_BINNED 0x073c /* 1852 */
/* Power-on frame length minus the full output height */
#define OV5647_VBLANK_MIN 24

/*
 * PLL constants for the RAW10 (10bpp) mode ov5647_init() selects -- see AUTHORIZED LOCAL
 * DIVERGENCE #2 in the file header for the lane-park/pad-drive bench evidence and
 * ov5647_init_regs[] below for where these actually get written to the sensor. Shared with that
 * register table so the programmed PLL and the declared pixel_rate can never drift apart:
 * VCO = XVCLK / OV5647_PLL_PREDIV * OV5647_PLL_MULT; lane bit rate = VCO / OV5647_PLL_SYS_DIV.
 *
 * SUPERSEDED (bench run 61, AUTHORIZED LOCAL DIVERGENCE #3): OV5647_PLL_MULT was 105 (VCO
 * 875 MHz, 437.5 Mbps/lane, matching mainline's link_freq for the FULL-RESOLUTION mode, 218750000)
 * from run 52's original PLL fix. Run 61 replaced it with the RPi/OmniVision reference value for
 * the 640x480 10bpp mode -- raspberrypi/linux rpi-6.6.y drivers/media/i2c/ov5647.c,
 * ov5647_640x480_10bpp[] -- giving OV5647_PLL_MULT = 70 (0x46): VCO = 25 MHz / 3 * 70 =
 * 583.33 MHz, lane bit rate = 583.33 MHz / 2 = 291.67 Mbps/lane. CITATION CORRECTED (issue #2248
 * fix-up round 4): fix-up round 3 claimed this matches "that reference's own declared pixel_rate
 * (58333000)" -- it does not. raspberrypi/linux's rpi-6.6.y, rpi-6.1.y and rpi-6.12.y all declare
 * .pixel_rate = 55000000 for the 640x480 10bpp mode, not 58333000; no revision checked declares
 * 58333000 anywhere. OV5647_PIXEL_RATE (58333333, derived below from this PLL) is instead
 * BENCH-MATCHED: run 63 measured fps_x100 = 1501 (15.01 fps) at VTS 0x0833 (2099) / HTS 1852,
 * which is exactly what ov5647_frmrate_to_vts()'s formula gives from 58333333, not from 55000000.
 * The CSI-2 receiver's hsfrequency bin 20 ({300 MHz, 0x14}) is CONFIGURATION
 * derived from our own declared pixel_rate (video_get_csi_link_freq() -> phy->pll_fin), NOT an
 * independent receiver-side measurement of the real PLL -- see the file header's evidence note
 * above for why this is not cited as confirmation on its own. The actual evidence this PLL is
 * correct is that runs 61/62/63 streamed with zero CSI fatals at that bin; a genuine PLL/bin
 * mismatch shows up as ERRSOTSYNCHS or PHY_FATAL (the same failure signature DIVERGENCE #2 above
 * bisected), not silent success.
 *
 * ONE GLOBAL PLL, deliberately, for both the 640x480 binned mode and the crop path: this file has
 * one ov5647_init_regs[] PLL write, not a per-mode PLL swap, so the crop path now also runs at
 * 291.67 Mbps/lane -- slower than mainline's OWN declared PLL for its full-resolution crop mode
 * (which run 52's 875 MHz VCO approximated, not run 61's). That is safe (a lower bit rate cannot
 * exceed the receiver's timing budget) but UNVERIFIED against mainline's actual crop-mode PLL
 * constants; only the 640x480 binned mode has bench evidence for this exact PLL (run 61). The
 * crop path's HTS stays the independently bench-proven OV5647_HTS_CROP (run 52, unchanged) --
 * see the per-mode HTS split at OV5647_HTS_640X480_BINNED below.
 */
#define OV5647_PLL_PREDIV  3
#define OV5647_PLL_MULT    70
#define OV5647_PLL_SYS_DIV 2

/*
 * pixel_rate = lane bit rate * lanes / bpp = (XVCLK / sysdiv * mult / prediv) * 2 / 10 (divide by
 * sysdiv first, not prediv, to stay exact and inside int32 range for every XVCLK in
 * [OV5647_INPUT_CLK_MIN, OV5647_INPUT_CLK_MAX]). At 25 MHz XVCLK: 58333333 -- DERIVED from this
 * driver's own PLL constants above, not copied from the RPi/OmniVision reference (which declares
 * 55000000 for this mode, not 58333333 -- see the PLL constants comment above for the correction),
 * and BENCH-MATCHED at run 63 (fps_x100 = 1501 at VTS 0x0833 / HTS 1852). Feeds BOTH
 * VIDEO_CID_PIXEL_RATE and ov5647_frmrate_to_vts() below -- TIMING_VTS is now derived per the
 * ACTIVE mode's HTS (OV5647_HTS_640X480_BINNED or OV5647_HTS_CROP), not one constant; see
 * ov5647_hts_for() near ov5647_set_mode_regs().
 *
 * HISTORY: an earlier attempt to "correct" this macro alone, in isolation, to a PLL-derived
 * 175000000 (issue #2248) was reverted, then run 52 fixed it properly with OV5647_PLL_MULT = 105
 * (875 MHz VCO, 437.5 Mbps/lane) -- matched to mainline's declared constants for its
 * FULL-RESOLUTION mode, not the 640x480 mode this driver actually ships. Run 61 (this comment)
 * supersedes that: PLL_MULT = 70, matched to mainline's OWN 640x480 10bpp table instead of its
 * full-resolution one, per bench evidence in the PLL constants comment above.
 *
 * NOT bench-verified for 8-bit: cfg->pixel_rate below is a single value computed once from DT at
 * OV5647_INIT() time, not re-derived per selected format. ov5647_set_fmt() does reprogram 0x3034's
 * OV5647_MIPI_BIT_MODE field per format (RAW8 vs RAW10), but nothing here recomputes pixel_rate to
 * match, so selecting VIDEO_PIX_FMT_SBGGR8 (8bpp) still reports the RAW10 value. Video_get_csi_
 * link_freq()'s PIXEL_RATE fallback (zephyr/drivers/video/video_common.c) computes link_freq =
 * pixel_rate * bpp / (2*lanes); at the same 583.33 MHz VCO, 8bpp would need pixel_rate =
 * 291.67e6 * 2 / 8 = 72916666, not 58333333, to feed that formula correctly. Only RAW10 is
 * bench-proven (runs 52/61) -- flagging this gap, not shipping an unverified 8-bit number.
 */
#define OV5647_PIXEL_RATE(clk) \
	((clk) / OV5647_PLL_SYS_DIV * OV5647_PLL_MULT / OV5647_PLL_PREDIV * 2 / \
	 OV5647_MIPI_BIT_MODE_RAW10)
#define OV5647_INPUT_CLK_MIN MHZ(6)
#define OV5647_INPUT_CLK_MAX MHZ(27)

#define OV5647_REG8(addr)  ((addr) | VIDEO_REG_ADDR16_DATA8)
#define OV5647_REG16(addr) ((addr) | VIDEO_REG_ADDR16_DATA16_BE)
#define OV5647_REG24(addr) ((addr) | VIDEO_REG_ADDR16_DATA24_BE)

#define OV5647_MODE_SELECT           OV5647_REG8(0x0100)
#define OV5647_MODE_SELECT_STREAMING BIT(0)
#define OV5647_SOFTWARE_RESET        OV5647_REG8(0x0103)
#define OV5647_SOFTWARE_RESET_RESET  BIT(0)
#define OV5647_CHIP_ID_REG           OV5647_REG16(0x300a)
#define OV5647_SC_MIPI_PHY           OV5647_REG8(0x3016)
#define OV5647_MIPI_PAD_ENABLE       BIT(3)
/* 0x3018 (MIPI PHY/route control) is now a literal write in ov5647_init_regs[] (run 61's
 * reference byte 0x44), not a targeted bitfield modify -- no local mask constants needed.
 */
#define OV5647_SC_MIPI_SC_CTRL OV5647_REG8(0x3018)
#define OV5647_SC_PLL_CTRL0    OV5647_REG8(0x3034)
#define OV5647_MIPI_BIT_MODE   GENMASK(3, 0)
#define OV5647_EXPOSURE        OV5647_REG24(0x3500)
#define OV5647_EXPOSURE_MAX    GENMASK(19, 0)
/*
 * AUTHORIZED LOCAL DIVERGENCE #3: the upstream default, 0x20 (2 lines in 1/16-line units), is
 * effectively a closed shutter for any manual-exposure user. 0x0FFF (~256 lines) matches the
 * order of magnitude of Alif's own shipped default (~0x000FFF) for this exact silicon.
 */
#define OV5647_EXPOSURE_DEFAULT     0x0FFF
#define OV5647_MANUAL_CTRL          OV5647_REG8(0x3503)
#define OV5647_MANUAL_CTRL_VTS      BIT(2)
#define OV5647_MANUAL_CTRL_AGC      BIT(1)
#define OV5647_MANUAL_CTRL_AEC      BIT(0)
#define OV5647_AGC_GAIN             OV5647_REG16(0x350a)
#define OV5647_AGC_GAIN_MAX         GENMASK(9, 0)
#define OV5647_VTS_DIFF             OV5647_REG16(0x350c)
#define OV5647_TIMING_X_ADDR_START  OV5647_REG16(0x3800)
#define OV5647_TIMING_Y_ADDR_START  OV5647_REG16(0x3802)
#define OV5647_TIMING_X_ADDR_END    OV5647_REG16(0x3804)
#define OV5647_TIMING_Y_ADDR_END    OV5647_REG16(0x3806)
#define OV5647_TIMING_X_OUTPUT_SIZE OV5647_REG16(0x3808)
#define OV5647_TIMING_Y_OUTPUT_SIZE OV5647_REG16(0x380a)
#define OV5647_TIMING_HTS_REG       OV5647_REG16(0x380c)
#define OV5647_TIMING_VTS_REG       OV5647_REG16(0x380e)
#define OV5647_TIMING_TC_REG20      OV5647_REG8(0x3820)
#define OV5647_TC_REG20_VFLIP       (BIT(2) | BIT(1))
#define OV5647_TIMING_TC_REG21      OV5647_REG8(0x3821)
#define OV5647_TC_REG21_MIRROR      (BIT(2) | BIT(1))

/*
 * Subsample (0x3814/0x3815) and binning-enable bits of 0x3820/0x3821 (AUTHORIZED LOCAL
 * DIVERGENCE #3, issue #2248, bench runs 56/58/60/61 -- run 61 is current, see the file header)
 * -- see the file header for the full write-up. ov5647_set_mode_regs() below writes the whole
 * window/output/subsample/binning/HTS set as ONE coherent block per mode: binning is only
 * coherent with the full-array window (0x3814 = 0x35 is a ~/4 decimation), and bench run 54
 * proved leaving it armed on a crop window throws CSI-2 frame-start/frame-end mismatches with no
 * image delivered -- see the ORDERING TRAP note in the file header. The 1:1 base value
 * (OV5647_TC_REG21_1TO1 = 0x00) leaves the mirror/flip bits (OV5647_TC_REG20_VFLIP /
 * OV5647_TC_REG21_MIRROR) at 0, but the binned mode's reference value (OV5647_TC_REG21_BINNED =
 * 0x01, run 61) does NOT -- its bits[2:1] are clear (mirror off) but bit0 is set; see the file
 * header's orientation note for what bits[2:1] control. ov5647_set_ctrl()'s HFLIP/VFLIP handlers
 * read-modify-write only bits[2:1], so writing a mode's base value here would silently undo a
 * previously requested mirror/flip -- ov5647_set_fmt() below re-applies both ctrls right after
 * this block (via ov5647_set_ctrl_hflip()/vflip()) on every format change, so the caller does NOT
 * need to reapply VIDEO_CID_HFLIP/VFLIP itself (issue #2248 fix-up round 3, see the forward
 * declaration above ov5647_set_fmt()).
 */
#define OV5647_TIMING_X_INC     OV5647_REG8(0x3814)
#define OV5647_TIMING_Y_INC     OV5647_REG8(0x3815)
#define OV5647_SUBSAMPLE_1TO1   0x11
#define OV5647_SUBSAMPLE_BINNED 0x35
#define OV5647_TC_REG20_1TO1    0x40
#define OV5647_TC_REG20_BINNED  0x41
#define OV5647_TC_REG21_1TO1    0x00
/* 0x01, not Alif's variant 0x07 -- run 61, see the file header's orientation note */
#define OV5647_TC_REG21_BINNED 0x01

/*
 * Analog registers that must track the subsample/binning mode above (same divergence, same
 * bench runs). 1:1 values are mainline Linux's full-resolution constants, cited but
 * BENCH-UNVERIFIED on this module; binned values are run-60's bench-proven binned-mode set.
 */
#define OV5647_ANALOG_CTRL12        OV5647_REG8(0x3612)
#define OV5647_ANALOG_CTRL12_1TO1   0x5b
#define OV5647_ANALOG_CTRL12_BINNED 0x59
#define OV5647_ANALOG_CTRL18        OV5647_REG8(0x3618)
#define OV5647_ANALOG_CTRL18_1TO1   0x04
#define OV5647_ANALOG_CTRL18_BINNED 0x00
#define OV5647_SENSOR_CTRL08        OV5647_REG8(0x3708)
#define OV5647_SENSOR_CTRL08_1TO1   0x64
#define OV5647_SENSOR_CTRL08_BINNED 0x64
#define OV5647_SENSOR_CTRL09        OV5647_REG8(0x3709)
#define OV5647_SENSOR_CTRL09_1TO1   0x12
#define OV5647_SENSOR_CTRL09_BINNED 0x52

/*
 * Full-array window read out by the 640x480 binned full-FOV mode (run 60) -- literal
 * bench-measured register values, NOT derived from OV5647_X_ADDR_START/OV5647_FULL_WIDTH like
 * ov5647_set_window()'s crop path below.
 */
#define OV5647_FULLFOV_X_ADDR_START 16
#define OV5647_FULLFOV_Y_ADDR_START 0
#define OV5647_FULLFOV_X_ADDR_END   0x0a2f
#define OV5647_FULLFOV_Y_ADDR_END   0x079f
#define OV5647_MODE_640X480_WIDTH   640
#define OV5647_MODE_640X480_HEIGHT  480

#define OV5647_ISP_CTRL3D          OV5647_REG8(0x503d)
#define OV5647_TEST_PATTERN_ENABLE BIT(7)

/*
 * PLL + MIPI-TX pad-drive registers (AUTHORIZED LOCAL DIVERGENCE #2, issue #2248, bench run 52) --
 * see the file header for the full write-up and evidence. ov5647_init_regs[] below writes all
 * eight of these while the sensor is still in software standby, immediately after the
 * OV5647_SOFTWARE_RESET write and before ov5647_set_fmt() -> ov5647_lane_park() puts it running
 * (0x0100 = 0x01): the PLL dividers latch at the 0x0103 software reset, NOT on standby exit, so
 * the same writes issued after the park (with 0x0100 already 0x01) update the register file
 * without moving the running PLL. Bench run 51 lost a cycle to exactly that ordering mistake -- do
 * not move this block after the OV5647_SC_MIPI_PHY entry below.
 */
#define OV5647_SC_PLL_CTRL1      OV5647_REG8(0x3035)
#define OV5647_SC_PLL_MULTIPLIER OV5647_REG8(0x3036)
#define OV5647_SC_PLL_CTRL3      OV5647_REG8(0x3037)
/*
 * 0x303c and 0x3106 are not documented in the datasheet register map available to us (same
 * situation as OV5647_PAD_OUT / 0x300d below); bench-confirmed values only, part of the same
 * run-52 write set.
 */
#define OV5647_SC_PLL_CTRL_RSVD_303C OV5647_REG8(0x303c)
#define OV5647_SC_CLKRST_RSVD_3106   OV5647_REG8(0x3106)

#define OV5647_IO_PAD_CTRL0          OV5647_REG8(0x3017)
#define OV5647_IO_PAD_CTRL0_PGM_LPTX GENMASK(5, 4) /* LP TX (data/clock lane) drive strength */
#define OV5647_IO_PAD_CTRL0_PGM_VCM  GENMASK(7, 6) /* common-mode voltage drive strength */
/* 0x301c/0x301d: undocumented, same as 0x303c/0x3106 above */
#define OV5647_IO_PAD_CTRL1 OV5647_REG8(0x301c)
#define OV5647_IO_PAD_CTRL2 OV5647_REG8(0x301d)

/*
 * Lane park/unpark sequence (mirrors mainline Linux's ov5647_power_on(), which calls
 * ov5647_stream_stop() under the comment "Stream off to coax lanes into LP-11 state"): with the
 * sensor left in bare software standby (0x0100 = 0x00) CSI_PHY_STOPSTATE reads back 0x00000000 --
 * no lane reaches LP-11. Writing 0x0100 = 0x01 (running) then these three registers, in this
 * order, parks the lanes at LP-11 (Stop state) so a CSI-2 receiver that waits for Stop-state
 * before stream start (e.g. a DesignWare CSI-2 host) can open the link. Bench-confirmed on an
 * E1M-AEN803 (serial 2026W36-0001) / E1M-EVK (hw_rev 2626-r2) with a DesignWare CSI-2 receiver:
 * 50/50 samples at 20 ms read CSI_PHY_STOPSTATE steadily as 0x00010003 (all three lanes -- CLK,
 * DATA_0, DATA_1) after this sequence, WITH ov5647_init_regs[]'s OV5647_IO_PAD_CTRL0 = 0xf0 write
 * also in effect (see DIVERGENCE #2 above). This park sequence alone, without that 0x3017 write,
 * only reaches 0x00000001 (DATA_0 only) -- an earlier version of this comment misdiagnosed that
 * gap as a hardware fault on the module under test. It was not: 0x3017 (LP TX pad drive) was still
 * at its OV5647 power-on default, too low to bring CLOCK and DATA_1 to Stop state on this
 * receiver. See the OV5647_IO_PAD_CTRL0 bisection above.
 */
#define OV5647_MIPI_CTRL00                    OV5647_REG8(0x4800)
#define OV5647_MIPI_CTRL00_CLOCK_LANE_GATE    BIT(5)
#define OV5647_MIPI_CTRL00_BUS_IDLE           BIT(2)
#define OV5647_MIPI_CTRL00_CLOCK_LANE_DISABLE BIT(0)
#define OV5647_FRAME_OFF_NUM                  OV5647_REG8(0x4202)
#define OV5647_FRAME_OFF_NUM_PARKED           0x0f
#define OV5647_FRAME_OFF_NUM_STREAMING        0x00
/*
 * Address 0x300d is not documented in the datasheet register map available to us; these are the
 * bench-confirmed values needed alongside MIPI_CTRL00 and FRAME_OFF_NUM to complete the
 * park/unpark sequence. Mainline names this register OV5640_REG_PAD_OUT -- matching that name here
 * makes re-applying this fix upstream (see the file header's retirement note) mechanical.
 */
#define OV5647_PAD_OUT           OV5647_REG8(0x300d)
#define OV5647_PAD_OUT_PARKED    0x01
#define OV5647_PAD_OUT_STREAMING 0x00

/*
 * Datasheet table 7-1 describes the 0x3034 bit mode field as 0 for 8-bit and 1 for 10-bit, which
 * does not match its own 0x1A power-on value for this 10-bit sensor: the field holds the number of
 * bits, as it does on the rest of the family.
 */
#define OV5647_MIPI_BIT_MODE_RAW8  8
#define OV5647_MIPI_BIT_MODE_RAW10 10

struct ov5647_config {
	struct i2c_dt_spec i2c;
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(pwdn_gpios)
	struct gpio_dt_spec pwdn_gpio;
#endif
	uint32_t pixel_rate;
};

struct ov5647_ctrls {
	/* auto_gain and gain are clustered together and must stay adjacent */
	struct video_ctrl auto_gain;
	struct video_ctrl gain;
	struct video_ctrl exposure_auto;
	struct video_ctrl exposure;
	struct video_ctrl hflip;
	struct video_ctrl vflip;
	struct video_ctrl test_pattern;
	struct video_ctrl pixel_rate;
};

struct ov5647_data {
	struct ov5647_ctrls ctrls;
	struct video_format fmt;
	/* ACTIVE (possibly clamped) frame rate -- what ov5647_get_frmival() reports and what
	 * OV5647_TIMING_VTS_REG was last programmed for. NOT what ov5647_set_fmt() should
	 * re-request on the next format change -- see requested_frmival below (issue #2248
	 * fix-up round 3: FIXED, the frame rate used to stick after a format change because
	 * set_fmt() fed this clamped value back in as the new request).
	 */
	uint32_t frmrate;
	/* USER-requested frame interval -- the WHOLE struct (numerator AND denominator), set ONLY
	 * by ov5647_set_frmival(), and ONLY after video_closest_frmival() and the VTS write have
	 * both succeeded (issue #2248 fix-up round 4: FIXED, a failed set_frmival() used to
	 * overwrite this anyway, so a rejected request still silently became the next format
	 * change's request). ov5647_set_fmt() re-requests THIS whole struct on every format change,
	 * not frmrate, so e.g. set_format(2592x1944) (which clamps the rate to 10 fps -- HTS_CROP
	 * can't sustain 15 fps at that height) followed by set_format(640x480) still asks for the
	 * original request again, rather than staying stuck at 10.
	 *
	 * MUST be the whole struct, not just the denominator (issue #2248 fix-up round 4: FIXED, an
	 * earlier version of this field was a bare uint32_t denominator, which silently turned a
	 * numerator != 1 request -- e.g. {2, 60}, 30 fps -- into a DIFFERENT rate {1, 60}, 60 fps,
	 * on the next format change; Zephyr's own `video frmival` shell command sends requests this
	 * way, e.g. `frmival <dev> 100ms` is {100, 1000}).
	 */
	struct video_frmival requested_frmival;
	/* Tracks the APPLICATION's streaming request (set_stream), not OV5647_MODE_SELECT, which
	 * ov5647_lane_park() also drives while parked/unparked and which this field does not mirror.
	 */
	bool streaming;
};

/*
 * Common init (AUTHORIZED LOCAL DIVERGENCE #3, issue #2248, bench run 61, verified as shipped by
 * run 62 -- "[stg] 75 regs 0 failed", MFR start ALP_OK, zero E: lines / CSI fatals, frame
 * CRC-matched). The register VALUES here are taken as hardware facts from the RPi/OmniVision
 * reference driver (raspberrypi/linux branch rpi-6.6.y, drivers/media/i2c/ov5647.c,
 * ov5647_common_regs[], GPL-2.0 -- see the file header for the copyright/license note), minus
 * 0x0100/0x0103 (this driver's own park/reset own those), minus 0x3017 (kept at OUR 0xf0, see
 * AUTHORIZED LOCAL DIVERGENCE #1/#2 above), minus 0x3503 (left to
 * ov5647_set_ctrl_exposure()'s existing OV5647_MANUAL_CTRL logic), and minus the mode-specific
 * 50/60 Hz AEC band-step registers (0x3a08/0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e, 0x4004 -- moved to
 * ov5647_set_mode_regs(), see the file header). None of these addresses is documented in the
 * datasheet register map available to us, so they keep the file's existing RSVD_<addr> naming
 * (see OV5647_SC_PLL_CTRL_RSVD_303C above) rather than a guessed semantic name. SUPERSEDES bench
 * run 58's Alif-derived block (runs 56/58/60): that block matched this one for most addresses
 * but diverged at 0x3821 (mode-specific, not common -- Alif 0x07 vs the reference's 0x01, see
 * the file header's orientation note) and at the ISP block enables, which Alif's table does NOT
 * omit -- it writes a DIFFERENT, larger set (0x5000=0x06, 0x5001=0x01, 0x5002=0x41, 0x5003=0x08,
 * 0x5a00=0x08) than the reference (0x5000/0x5003/0x5a00 only). Run 58's CSI "incorrect frame
 * sequence" fatals came from that larger set specifically (0x5001/0x5002, and separately
 * 0x4050/0x4051 in runs 56/58/60's now-superseded BLC section), not from ISP enables as a
 * category. This block INCLUDES the reference's three (0x5000/0x5003/0x5a00) and OMITS the four
 * Alif-only additions (0x5001/0x5002/0x4050/0x4051) -- do not add those four back.
 */
#define OV5647_SYSTEM_RSVD_3000 OV5647_REG8(0x3000)
#define OV5647_SYSTEM_RSVD_3001 OV5647_REG8(0x3001)
#define OV5647_SYSTEM_RSVD_3002 OV5647_REG8(0x3002)
#define OV5647_ANALOG_RSVD_370C OV5647_REG8(0x370c)
#define OV5647_ANALOG_RSVD_3630 OV5647_REG8(0x3630)
#define OV5647_ANALOG_RSVD_3632 OV5647_REG8(0x3632)
#define OV5647_ANALOG_RSVD_3633 OV5647_REG8(0x3633)
#define OV5647_ANALOG_RSVD_3634 OV5647_REG8(0x3634)
#define OV5647_ANALOG_RSVD_3620 OV5647_REG8(0x3620)
#define OV5647_ANALOG_RSVD_3621 OV5647_REG8(0x3621)
#define OV5647_ANALOG_RSVD_3600 OV5647_REG8(0x3600)
#define OV5647_ANALOG_RSVD_3704 OV5647_REG8(0x3704)
#define OV5647_ANALOG_RSVD_3703 OV5647_REG8(0x3703)
#define OV5647_ANALOG_RSVD_3715 OV5647_REG8(0x3715)
#define OV5647_ANALOG_RSVD_3717 OV5647_REG8(0x3717)
#define OV5647_ANALOG_RSVD_3731 OV5647_REG8(0x3731)
#define OV5647_ANALOG_RSVD_370B OV5647_REG8(0x370b)
#define OV5647_ANALOG_RSVD_3705 OV5647_REG8(0x3705)
#define OV5647_ANALOG_RSVD_3F05 OV5647_REG8(0x3f05)
#define OV5647_ANALOG_RSVD_3F06 OV5647_REG8(0x3f06)
#define OV5647_ANALOG_RSVD_3F01 OV5647_REG8(0x3f01)
#define OV5647_ANALOG_RSVD_3C01 OV5647_REG8(0x3c01)
#define OV5647_ANALOG_RSVD_3B07 OV5647_REG8(0x3b07)
#define OV5647_ANALOG_RSVD_3636 OV5647_REG8(0x3636)
#define OV5647_ANALOG_RSVD_3827 OV5647_REG8(0x3827)
#define OV5647_BLC_RSVD_4001    OV5647_REG8(0x4001)
#define OV5647_BLC_RSVD_4004    OV5647_REG8(0x4004)
#define OV5647_BLC_RSVD_4000    OV5647_REG8(0x4000)
#define OV5647_AEC_RSVD_3A18    OV5647_REG8(0x3a18)
#define OV5647_AEC_RSVD_3A19    OV5647_REG8(0x3a19)
#define OV5647_AEC_RSVD_3A08    OV5647_REG8(0x3a08)
#define OV5647_AEC_RSVD_3A09    OV5647_REG8(0x3a09)
#define OV5647_AEC_RSVD_3A0A    OV5647_REG8(0x3a0a)
#define OV5647_AEC_RSVD_3A0B    OV5647_REG8(0x3a0b)
#define OV5647_AEC_RSVD_3A0D    OV5647_REG8(0x3a0d)
#define OV5647_AEC_RSVD_3A0E    OV5647_REG8(0x3a0e)

/*
 * Crop-path 50/60 Hz AEC band-step VALUES (issue #2248 fix-up round 5), line-time-scaled from
 * mainline's full-resolution table -- see the block comment in ov5647_set_mode_regs() for the
 * arithmetic. Named here (not just inline in the register table) because ov5647_set_mode_regs()
 * also needs them as divisors to compute the PER-MODE max-bands figures written to
 * OV5647_AEC_RSVD_3A0D/3A0E -- see that computation for why a single full-resolution-derived
 * constant (the driver's earlier bug) is wrong for a smaller crop.
 */
#define OV5647_AEC_BAND_50HZ_CROP 208U /* 0x3a08/0x3a09 = 0x00/0xd0 */
#define OV5647_AEC_BAND_60HZ_CROP 173U /* 0x3a0a/0x3a0b = 0x00/0xad */
#define OV5647_AEC_RSVD_3A0F      OV5647_REG8(0x3a0f)
#define OV5647_AEC_RSVD_3A10      OV5647_REG8(0x3a10)
#define OV5647_AEC_RSVD_3A1B      OV5647_REG8(0x3a1b)
#define OV5647_AEC_RSVD_3A1E      OV5647_REG8(0x3a1e)
#define OV5647_AEC_RSVD_3A11      OV5647_REG8(0x3a11)
#define OV5647_AEC_RSVD_3A1F      OV5647_REG8(0x3a1f)
/* ISP block enables -- run 61, part of RPi's ov5647_common_regs[]; see the block comment above
 * for why 0x5001/0x5002 (Alif-only, run-58 CSI fatals) are deliberately NOT declared here.
 */
#define OV5647_ISP_RSVD_5000 OV5647_REG8(0x5000)
#define OV5647_ISP_RSVD_5003 OV5647_REG8(0x5003)
#define OV5647_ISP_RSVD_5A00 OV5647_REG8(0x5a00)

static const struct video_reg ov5647_init_regs[] = {
	/* PLL + MIPI-TX pad-drive init -- see the block comment above OV5647_SC_PLL_CTRL1 for why
	 * this must run here (software standby, before ov5647_lane_park()) and not be reordered.
	 */
	{ OV5647_SC_PLL_CTRL3, OV5647_PLL_PREDIV },    /* bits[3:0] prediv=3, bit4 root_div=0 (/1) */
	{ OV5647_SC_PLL_MULTIPLIER, OV5647_PLL_MULT }, /* full-byte multiplier = 70 (0x46), run 61 */
	{ OV5647_SC_PLL_CTRL1, (OV5647_PLL_SYS_DIV << 4) | 0x1 }, /* bits[7:4] sysdiv=2; low nibble
								  * unchanged from power-on (0x11)
								  */
	{ OV5647_SC_PLL_CTRL_RSVD_303C, 0x11 },
	{ OV5647_IO_PAD_CTRL0, 0xf0 }, /* pgm_lptx=3 (max, bits[5:4]), pgm_vcm=3 (bits[7:6]); see the
				      * file header's bisection for why pgm_lptx must be 3, not
				      * mainline's 2
				      */
	{ OV5647_IO_PAD_CTRL1, 0xf8 },
	{ OV5647_IO_PAD_CTRL2, 0xf0 },
	{ OV5647_SC_CLKRST_RSVD_3106, 0xf5 },
	{ OV5647_SC_MIPI_PHY, OV5647_MIPI_PAD_ENABLE },
	/* Drive the frame length from TIMING_VTS instead of letting the AEC stretch it. HTS is now
	 * PER MODE (AUTHORIZED LOCAL DIVERGENCE #3, run 61) -- ov5647_set_mode_regs() writes
	 * OV5647_TIMING_HTS_REG as part of each mode's coherent register block, not here.
	 *
	 * #2277: the VTS-manual bit itself (OV5647_MANUAL_CTRL_VTS, OV5647_MANUAL_CTRL = 0x3503)
	 * is deliberately NOT a table entry here -- a flat {reg, value} table entry is a WHOLE-BYTE
	 * overwrite (video_write_cci_multiregs()), which would silently also clear the AEC/AGC-
	 * manual bits this same register holds, the moment anything ever needed to re-run this
	 * table after ctrls exist. ov5647_init() below sets just this one bit with an explicit
	 * video_modify_cci_reg() read-modify-write instead, right after this table -- same net
	 * effect at boot (the register reads 0 post-reset either way) but safe if this table is
	 * ever re-applied later.
	 */
	{ OV5647_VTS_DIFF, 0 },
	/* Common init (AUTHORIZED LOCAL DIVERGENCE #3, run 61) -- see the block comment above
	 * OV5647_SYSTEM_RSVD_3000 for the source and what was deliberately left out.
	 */
	{ OV5647_SYSTEM_RSVD_3000, 0x00 },
	{ OV5647_SYSTEM_RSVD_3001, 0x00 },
	{ OV5647_SYSTEM_RSVD_3002, 0x00 },
	{ OV5647_SC_MIPI_SC_CTRL, 0x44 }, /* MIPI_EN set, PHY_PD_MIPI/PHY_PD_LPRX clear, per RPi's
					  * literal reference byte -- supersedes the old
					  * bitfield-only video_modify_cci_reg() call below
					  */
	{ OV5647_ANALOG_RSVD_370C, 0x03 },
	{ OV5647_ANALOG_RSVD_3630, 0x2e },
	{ OV5647_ANALOG_RSVD_3632, 0xe2 },
	{ OV5647_ANALOG_RSVD_3633, 0x23 },
	{ OV5647_ANALOG_RSVD_3634, 0x44 },
	{ OV5647_ANALOG_RSVD_3620, 0x64 },
	{ OV5647_ANALOG_RSVD_3621, 0xe0 },
	{ OV5647_ANALOG_RSVD_3600, 0x37 },
	{ OV5647_ANALOG_RSVD_3704, 0xa0 },
	{ OV5647_ANALOG_RSVD_3703, 0x5a },
	{ OV5647_ANALOG_RSVD_3715, 0x78 },
	{ OV5647_ANALOG_RSVD_3717, 0x01 },
	{ OV5647_ANALOG_RSVD_3731, 0x02 },
	{ OV5647_ANALOG_RSVD_370B, 0x60 },
	{ OV5647_ANALOG_RSVD_3705, 0x1a },
	{ OV5647_ANALOG_RSVD_3F05, 0x02 },
	{ OV5647_ANALOG_RSVD_3F06, 0x10 },
	{ OV5647_ANALOG_RSVD_3F01, 0x0a },
	{ OV5647_ANALOG_RSVD_3C01, 0x80 },
	{ OV5647_ANALOG_RSVD_3B07, 0x0c },
	{ OV5647_ANALOG_RSVD_3636, 0x06 },
	{ OV5647_ANALOG_RSVD_3827, 0xec },
	/* BLC (run 61; matches bench run 58's earlier values). 0x4004 moved to
	 * ov5647_set_mode_regs() -- see the AEC band-step note below, same reason.
	 */
	{ OV5647_BLC_RSVD_4001, 0x02 },
	{ OV5647_BLC_RSVD_4000, 0x09 },
	/* AEC target/limits (run 61; matches bench run 58's earlier values); left unchanged --
	 * see the file header's low-light note, a scene limitation, not a driver bug.
	 * 0x3a08/0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e (50/60 Hz band step, in LINES) moved to
	 * ov5647_set_mode_regs() because a line count depends on line TIME -- HTS_CROP (2700) is a
	 * different line time than HTS_640X480_BINNED (1852), so a value correct for one mode is
	 * wrong for the other. CITATION CORRECTED (issue #2248 fix-up round 4): fix-up round 3
	 * claimed "the reference declares them per mode", i.e. in ov5647_640x480_10bpp[] /
	 * ov5647_2592x1944_10bpp[] -- true for 0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e, but NOT for
	 * 0x3a08: in raspberrypi/linux rpi-6.6.y drivers/media/i2c/ov5647.c, 0x3a08 = 0x01 is in
	 * ov5647_common_regs[], and ov5647_2592x1944_10bpp[] there has no 0x3a08 entry at all (only
	 * upstream mainline Linux 6.6's own 2592x1944 table declares 0x3a08 per mode). Moving 0x3a08
	 * out of the common block here is still correct -- fix-up round 3 was right that it is
	 * mode-independent in VALUE only by coincidence (0x01 in both this driver's modes) and not
	 * in KIND, so a future mode with a genuinely different 0x3a08 must not find it pinned here --
	 * but the RPi-reference citation for doing so was wrong; only mainline 6.6 supports it.
	 * Common-block placement here would have silently carried the binned mode's line counts
	 * into the crop path.
	 */
	{ OV5647_AEC_RSVD_3A18, 0x00 },
	{ OV5647_AEC_RSVD_3A19, 0xf8 },
	{ OV5647_AEC_RSVD_3A0F, 0x58 },
	{ OV5647_AEC_RSVD_3A10, 0x50 },
	{ OV5647_AEC_RSVD_3A1B, 0x58 },
	{ OV5647_AEC_RSVD_3A1E, 0x50 },
	{ OV5647_AEC_RSVD_3A11, 0x60 },
	{ OV5647_AEC_RSVD_3A1F, 0x28 },
	/* ISP block enables (run 61) -- see the block comment above OV5647_ISP_RSVD_5000 for why
	 * 0x5001/0x5002 are deliberately NOT here.
	 */
	{ OV5647_ISP_RSVD_5000, 0x06 },
	{ OV5647_ISP_RSVD_5003, 0x08 },
	{ OV5647_ISP_RSVD_5A00, 0x08 },
};

enum ov5647_fmt_id {
	OV5647_FMT_SBGGR8,
	OV5647_FMT_SBGGR10P,
};

static const struct video_format_cap ov5647_fmts[] = {
	[OV5647_FMT_SBGGR8] = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width_min = 4, .width_max = OV5647_FULL_WIDTH, .width_step = 4,
		.height_min = 4, .height_max = OV5647_FULL_HEIGHT, .height_step = 4,
	},
	[OV5647_FMT_SBGGR10P] = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width_min = 4, .width_max = OV5647_FULL_WIDTH, .width_step = 4,
		.height_min = 4, .height_max = OV5647_FULL_HEIGHT, .height_step = 4,
	},
	{0},
};

/* Frame rates from datasheet table 2-1, plus a slower one usable at any resolution */
static const uint32_t ov5647_framerates[] = { 10, 15, 30, 45, 60, 90, 120 };

/*
 * Which HTS applies to @p width x @p height (AUTHORIZED LOCAL DIVERGENCE #3, bench run 61):
 * OV5647_HTS_640X480_BINNED for exactly the 640x480 binned mode, OV5647_HTS_CROP for every
 * other (crop-path) size -- see the OV5647_HTS_640X480_BINNED/OV5647_HTS_CROP comment above for
 * why HTS is per-mode, not one driver-wide constant.
 */
static uint32_t ov5647_hts_for(uint32_t width, uint32_t height)
{
	if (width == OV5647_MODE_640X480_WIDTH && height == OV5647_MODE_640X480_HEIGHT) {
		return OV5647_HTS_640X480_BINNED;
	}

	return OV5647_HTS_CROP;
}

static uint32_t ov5647_frmrate_to_vts(const struct device *dev, uint32_t hts, uint32_t frmrate)
{
	const struct ov5647_config *cfg = dev->config;

	return cfg->pixel_rate / (hts * frmrate);
}

static int ov5647_set_window(const struct device *dev, uint32_t width, uint32_t height)
{
	const struct ov5647_config *cfg     = dev->config;
	uint32_t                    x_start = OV5647_X_ADDR_START + (OV5647_FULL_WIDTH - width) / 2;
	uint32_t                    y_start = OV5647_Y_ADDR_START + (OV5647_FULL_HEIGHT - height) / 2;
	const struct video_reg      regs[]  = {
		{ OV5647_TIMING_X_ADDR_START, x_start },
		{ OV5647_TIMING_Y_ADDR_START, y_start },
		{ OV5647_TIMING_X_ADDR_END, x_start + width + OV5647_WINDOW_MARGIN - 1 },
		{ OV5647_TIMING_Y_ADDR_END, y_start + height + OV5647_WINDOW_MARGIN - 1 },
		{ OV5647_TIMING_X_OUTPUT_SIZE, width },
		{ OV5647_TIMING_Y_OUTPUT_SIZE, height },
	};

	return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
}

/*
 * Pick the window/output-size/subsample/binning/analog/HTS/AEC-band-step register set for
 * @p width x @p height (AUTHORIZED LOCAL DIVERGENCE #3, issue #2248, bench runs 61/62) and write
 * it as ONE coherent block -- see the ORDERING TRAP note in the file header for why binning (and
 * its HTS/band step) must never be written apart from the full-array window it depends on.
 * 640x480 gets the RPi/OmniVision-reference full-FOV binned mode; every other size keeps today's
 * centred-crop window (ov5647_set_window()) but now also explicitly re-asserts the 1:1
 * subsample/binning/analog/HTS/band-step values, so a prior 640x480 selection can never leave
 * binning (or its HTS/band step) armed on the new crop window. The AEC band-step registers
 * (0x3a08/0x3a09/0x3a0a/0x3a0b/0x3a0d/0x3a0e, 0x4004) are here rather than in ov5647_init_regs[] because
 * they are LINE COUNTS, which depend on line time/HTS -- a value correct for one mode's HTS is
 * wrong for the other's.
 */
static int ov5647_set_mode_regs(const struct device *dev, uint32_t width, uint32_t height)
{
	const struct ov5647_config *cfg = dev->config;
	int                         ret;

	if (width == OV5647_MODE_640X480_WIDTH && height == OV5647_MODE_640X480_HEIGHT) {
		const struct video_reg regs[] = {
			{ OV5647_TIMING_X_ADDR_START, OV5647_FULLFOV_X_ADDR_START },
			{ OV5647_TIMING_Y_ADDR_START, OV5647_FULLFOV_Y_ADDR_START },
			{ OV5647_TIMING_X_ADDR_END, OV5647_FULLFOV_X_ADDR_END },
			{ OV5647_TIMING_Y_ADDR_END, OV5647_FULLFOV_Y_ADDR_END },
			{ OV5647_TIMING_X_OUTPUT_SIZE, width },
			{ OV5647_TIMING_Y_OUTPUT_SIZE, height },
			{ OV5647_TIMING_HTS_REG, OV5647_HTS_640X480_BINNED },
			{ OV5647_TIMING_X_INC, OV5647_SUBSAMPLE_BINNED },
			{ OV5647_TIMING_Y_INC, OV5647_SUBSAMPLE_BINNED },
			{ OV5647_TIMING_TC_REG21, OV5647_TC_REG21_BINNED },
			{ OV5647_TIMING_TC_REG20, OV5647_TC_REG20_BINNED },
			{ OV5647_ANALOG_CTRL12, OV5647_ANALOG_CTRL12_BINNED },
			{ OV5647_ANALOG_CTRL18, OV5647_ANALOG_CTRL18_BINNED },
			{ OV5647_SENSOR_CTRL08, OV5647_SENSOR_CTRL08_BINNED },
			{ OV5647_SENSOR_CTRL09, OV5647_SENSOR_CTRL09_BINNED },
			/* 50/60 Hz AEC band step, in LINES -- depends on HTS_640X480_BINNED's line
			 * time. Reference (ov5647_640x480_10bpp[]), bench-confirmed runs 61/62.
			 * 0x3a08 moved here from the common init too (issue #2248 fix-up round 3):
			 * same reason as the other four -- it is part of the same per-mode band-step
			 * group, not a mode-independent constant.
			 */
			{ OV5647_AEC_RSVD_3A08, 0x01 },
			{ OV5647_AEC_RSVD_3A09, 0x2e },
			{ OV5647_AEC_RSVD_3A0A, 0x00 },
			{ OV5647_AEC_RSVD_3A0B, 0xfb },
			{ OV5647_AEC_RSVD_3A0D, 0x02 },
			{ OV5647_AEC_RSVD_3A0E, 0x01 },
			{ OV5647_BLC_RSVD_4004, 0x02 },
		};

		return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
	}

	/*
	 * 0x3a0d/0x3a0e (max bands per frame) are PER MODE, not the single full-resolution-derived
	 * constant an earlier version of this fix hardcoded here for every crop size (issue #2248
	 * fix-up round 5): the reference's own floor(VTS/band) rule (see the block comment below)
	 * depends on the MODE's own minimum-blanking VTS (height + OV5647_VBLANK_MIN), so a value
	 * derived from OV5647_FULL_HEIGHT (1944) overstates what a SMALLER crop can actually hold
	 * -- e.g. 1280x960 (min VTS ~984): the full-resolution-derived 11 (1968/173, the 60 Hz band)
	 * and 9 (1968/208, the 50 Hz band) give 11*173=1903 and 9*208=1872, both exceed its
	 * minimum-blanking VTS. Recomputed per mode from the height actually requested here, floored
	 * at 1 band. BENCH-UNVERIFIED, same as the band-step values themselves (see below).
	 *
	 * Using height + OV5647_VBLANK_MIN (the mode's MINIMUM-blanking VTS) rather than the ACTUAL
	 * VTS the caller ends up running at (set separately, later, by ov5647_set_frmival(), which
	 * this function runs before) is a deliberate, conservative choice, not a remaining bug: at
	 * any rate slower than a mode's fastest reachable one the real VTS is larger, so bands
	 * computed from the minimum cap the AEC's usable exposure range more tightly than the
	 * hardware could actually support at that slower rate (roughly 60% of the frame at the crop
	 * path's default 15 fps) -- but the safe direction: it can never let the AEC exceed the
	 * frame's actual VTS, which the earlier OV5647_FULL_HEIGHT-derived bug this block replaces
	 * sometimes did. Upgrade path, if the conservatism ever needs tightening: re-derive and
	 * rewrite 0x3a0d/0x3a0e from the ACTUAL VTS inside ov5647_set_frmival() once that VTS is
	 * known, instead of the mode's minimum here.
	 */
	uint32_t crop_min_vts   = height + OV5647_VBLANK_MIN;
	uint32_t max_bands_50hz = MAX(1U, crop_min_vts / OV5647_AEC_BAND_50HZ_CROP);
	uint32_t max_bands_60hz = MAX(1U, crop_min_vts / OV5647_AEC_BAND_60HZ_CROP);

	ret = ov5647_set_window(dev, width, height);
	if (ret < 0) {
		return ret;
	}

	const struct video_reg regs[] = {
		{ OV5647_TIMING_HTS_REG, OV5647_HTS_CROP },
		{ OV5647_TIMING_X_INC, OV5647_SUBSAMPLE_1TO1 },
		{ OV5647_TIMING_Y_INC, OV5647_SUBSAMPLE_1TO1 },
		{ OV5647_TIMING_TC_REG20, OV5647_TC_REG20_1TO1 },
		{ OV5647_TIMING_TC_REG21, OV5647_TC_REG21_1TO1 },
		{ OV5647_ANALOG_CTRL12, OV5647_ANALOG_CTRL12_1TO1 },
		{ OV5647_ANALOG_CTRL18, OV5647_ANALOG_CTRL18_1TO1 },
		{ OV5647_SENSOR_CTRL08, OV5647_SENSOR_CTRL08_1TO1 },
		{ OV5647_SENSOR_CTRL09, OV5647_SENSOR_CTRL09_1TO1 },
		/* 50/60 Hz AEC band step, in LINES -- depends on line time, so the binned mode's
		 * values (HTS_640X480_BINNED = 1852) do not carry over to HTS_CROP (2700).
		 *
		 * issue #2248 fix-up round 4 (BENCH-UNVERIFIED, not run): fix-up round 3 wrote
		 * mainline's OWN full-resolution values here byte-for-byte (0x3a08/0x3a09 =
		 * 0x01/0x28 = 296 lines, 0x3a0a/0x3a0b = 0x00/0xf6 = 246 lines), citing
		 * raspberrypi/linux rpi-6.6.y drivers/media/i2c/ov5647.c,
		 * ov5647_2592x1944_10bpp[] -- but that reference declares pixel_rate 87500000
		 * with HTS 2844, a 32.51us line (2844 / 87500000), while THIS driver's crop path
		 * runs OV5647_HTS_CROP (2700) on OV5647_PIXEL_RATE (58333333), a 46.29us line
		 * (2700 / 58333333). A band step is a LINE COUNT standing in for a fixed
		 * real-time AC period, so copying the line count across a DIFFERENT line time
		 * changes the real time it represents (296 lines * 32.51us = 9.63 ms; the same
		 * 296 lines at THIS line time is 296 * 46.29us = 13.70 ms -- neither the 50 Hz
		 * nor 60 Hz mains period). Scaled by the line-time ratio to preserve the real-time
		 * period instead: 296 * (32.51 / 46.29) = 207.9 -> 208 (OV5647_AEC_BAND_50HZ_CROP);
		 * 246 * (32.51 / 46.29) = 172.8 -> 173 (OV5647_AEC_BAND_60HZ_CROP). 0x3a0d/0x3a0e
		 * (max bands per frame) follow the reference's own floor(VTS/band) rule -- mainline's
		 * 0x08/0x06 reproduce exactly from floor(1968/246)=8 and floor(1968/296)=6.65->6,
		 * where 1968 = OV5647_FULL_HEIGHT + OV5647_VBLANK_MIN is ITS mode's minimum-blanking
		 * VTS (this block runs once per mode, before ov5647_set_frmival() picks an actual
		 * rate, so the max-bands figure cannot depend on a chosen frame rate) -- but THIS
		 * driver supports more than one crop size, so max_bands_50hz/max_bands_60hz above use
		 * the REQUESTED height's own minimum-blanking VTS, not a constant pinned to
		 * OV5647_FULL_HEIGHT (issue #2248 fix-up round 5: fixed, see that computation's own
		 * comment). This scaling has NOT been bench-verified on this module -- flagging the
		 * derivation, not shipping it as measured.
		 */
		{ OV5647_AEC_RSVD_3A08, 0x00 },
		{ OV5647_AEC_RSVD_3A09, OV5647_AEC_BAND_50HZ_CROP },
		{ OV5647_AEC_RSVD_3A0A, 0x00 },
		{ OV5647_AEC_RSVD_3A0B, OV5647_AEC_BAND_60HZ_CROP },
		{ OV5647_AEC_RSVD_3A0D, max_bands_60hz },
		{ OV5647_AEC_RSVD_3A0E, max_bands_50hz },
		{ OV5647_BLC_RSVD_4004, 0x04 },
	};

	return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
}

static int ov5647_enum_frmival(const struct device *dev, struct video_frmival_enum *fie)
{
	uint32_t hts;

	if (fie->index >= ARRAY_SIZE(ov5647_framerates)) {
		return -EINVAL;
	}

	/* HTS depends on the mode @p fie->format names (AUTHORIZED LOCAL DIVERGENCE #3) -- see
	 * ov5647_hts_for().
	 */
	hts = ov5647_hts_for(fie->format->width, fie->format->height);

	/* A frame rate is only reachable if its frame length still covers the read-out */
	if (ov5647_frmrate_to_vts(dev, hts, ov5647_framerates[fie->index]) <
	    fie->format->height + OV5647_VBLANK_MIN) {
		return -EINVAL;
	}

	fie->type                 = VIDEO_FRMIVAL_TYPE_DISCRETE;
	fie->discrete.numerator   = 1;
	fie->discrete.denominator = ov5647_framerates[fie->index];

	return 0;
}

static int ov5647_set_frmival(const struct device *dev, struct video_frmival *frmival)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;
	/* The RAW request, exactly as the caller passed it (numerator AND denominator) -- kept
	 * aside so it can be saved into data->requested_frmival further down, but ONLY once
	 * video_closest_frmival() and the VTS write below have both succeeded (issue #2248 fix-up
	 * round 4, see requested_frmival's declaration). *frmival itself is overwritten with the
	 * CLAMPED result before this function returns, so it cannot be read back for that later.
	 */
	struct video_frmival      requested = *frmival;
	struct video_frmival_enum fie       = {
		.discrete = *frmival,
		.type     = VIDEO_FRMIVAL_TYPE_DISCRETE,
		.format   = &data->fmt,
	};
	uint32_t hts;
	int      ret;

	ret = video_closest_frmival(dev, &fie);
	if (ret < 0) {
		return ret;
	}

	/* data->fmt is the ACTIVE mode (ov5647_set_fmt() updates it before calling here) --
	 * AUTHORIZED LOCAL DIVERGENCE #3, see ov5647_hts_for().
	 */
	hts = ov5647_hts_for(data->fmt.width, data->fmt.height);

	ret = video_write_cci_reg(&cfg->i2c,
	                          OV5647_TIMING_VTS_REG,
	                          ov5647_frmrate_to_vts(dev, hts, ov5647_framerates[fie.index]));
	if (ret < 0) {
		return ret;
	}

	/* Only now that both steps above succeeded: store the caller's WHOLE raw request for
	 * ov5647_set_fmt() to re-request on the next format change -- see requested_frmival's
	 * declaration. A rejected/failed request must not overwrite a previously-saved one.
	 */
	data->requested_frmival = requested;

	/*
	 * Report the rate actually just written to OV5647_TIMING_VTS_REG above, not fie.discrete
	 * (issue #2248 fix-up round 5, pre-existing bug): Zephyr's video_closest_frmival() tracks
	 * the best candidate by comparing each diff_nsec against a running best (initialised to
	 * INT32_MAX ns) and only updates match->discrete/index when a candidate beats it -- if
	 * EVERY candidate's diff from the request exceeds INT32_MAX ns (e.g. a {5, 1} request, 5 s,
	 * against this driver's fastest 10 fps candidate, ~4.9 s away), no candidate ever updates
	 * fie.discrete/fie.index, yet video_closest_frmival() still returns 0 -- fie.discrete is
	 * left at the caller's original raw request and fie.index at its zero-initialised default.
	 * Echoing fie.discrete here would then report the UNAPPLIED raw request while
	 * ov5647_framerates[fie.index] (index 0) is what the VTS write above actually used.
	 * {1, ov5647_framerates[fie.index]} is exactly what ov5647_enum_frmival() itself would
	 * report for that same index, so this is a no-op on every request video_closest_frmival()
	 * DID match normally, and only changes behaviour on this one previously-silent path.
	 */
	data->frmrate = ov5647_framerates[fie.index];
	*frmival      = (struct video_frmival){ .numerator = 1, .denominator = data->frmrate };

	return 0;
}

static int ov5647_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct ov5647_data *data = dev->data;

	frmival->numerator   = 1;
	frmival->denominator = data->frmrate;

	return 0;
}

/* Forward declaration: ov5647_set_fmt() re-parks after its writes (see below), but the park
 * helper is defined further down, alongside the registers it uses.
 */
static int ov5647_lane_park(const struct device *dev);

/* Forward declarations: ov5647_set_fmt() re-applies both flip ctrls after ov5647_set_mode_regs()
 * (see below) -- issue #2248 fix-up round 3, FIXED: ov5647_set_mode_regs() writes 0x3820/0x3821
 * as whole bytes (AUTHORIZED LOCAL DIVERGENCE #3), which silently wiped
 * OV5647_TC_REG20_VFLIP/OV5647_TC_REG21_MIRROR bits a caller had set via
 * VIDEO_CID_VFLIP/VIDEO_CID_HFLIP on every format change, while the ctrl itself kept reporting
 * the value the caller set -- ctrl and hardware silently diverged. Defined further down,
 * alongside the other per-ctrl setters.
 */
static int ov5647_set_ctrl_hflip(const struct device *dev);
static int ov5647_set_ctrl_vflip(const struct device *dev);

/* Forward declaration: ov5647_set_fmt() below both ACCEPTS and, on success, REPORTS the
 * flip-adjusted fourcc this returns -- issue #2248 fix-up round 5, see set_fmt()'s own comment.
 * Defined further down, next to ov5647_get_fmt() which uses it the same way.
 */
static uint32_t ov5647_bayer_pixfmt(uint32_t base_pixelformat, bool hflip, bool vflip);

static int ov5647_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;
	/* Re-request the WHOLE struct the USER asked for, not data->frmrate (the last EFFECTIVE,
	 * possibly clamped, rate) -- issue #2248 fix-up round 3, see requested_frmival's
	 * declaration. Using data->frmrate here was the bug: set_format(2592x1944) clamps frmrate
	 * to 10 (HTS_CROP can't sustain 15 fps at that height), and a later set_format(640x480) fed
	 * that clamped 10 back in as the new request, silently never recovering the original rate
	 * even though 640x480 can reach it.
	 */
	struct video_frmival frmival               = data->requested_frmival;
	bool                 hflip                 = data->ctrls.hflip.val != 0;
	bool                 vflip                 = data->ctrls.vflip.val != 0;
	uint32_t             requested_pixelformat = fmt->pixelformat;
	size_t               idx;
	int                  ret;

	if (data->streaming) {
		LOG_ERR("Cannot change the format while streaming");
		return -EBUSY;
	}

	/*
	 * ov5647_get_fmt() reports the EFFECTIVE (flip-shifted) Bayer fourcc, not just the two BASE
	 * ones ov5647_fmts[] advertises -- issue #2248 fix-up round 5 (MAJOR BUG): a
	 * get_format() -> set_format() round trip, or any caller re-submitting exactly what
	 * get_format() just returned, used to fail here with -ENOTSUP, because
	 * video_format_caps_index() only ever matches the two base fourccs. Map back to the base
	 * fourcc when the request equals what the CURRENT flip state derives from one of them --
	 * ov5647_bayer_pixfmt() is its own inverse for a fixed flip state, so this only ever fires
	 * on the exact fourcc get_format() would report right now. A caller submitting a base
	 * fourcc directly is unaffected: it only matches this check when hflip/vflip are both
	 * unset (ov5647_bayer_pixfmt()'s (0,0) case is the identity).
	 */
	if (requested_pixelformat == ov5647_bayer_pixfmt(VIDEO_PIX_FMT_SBGGR8, hflip, vflip)) {
		fmt->pixelformat = VIDEO_PIX_FMT_SBGGR8;
	} else if (requested_pixelformat == ov5647_bayer_pixfmt(VIDEO_PIX_FMT_SBGGR10P, hflip, vflip)) {
		fmt->pixelformat = VIDEO_PIX_FMT_SBGGR10P;
	}

	ret = video_format_caps_index(ov5647_fmts, fmt, &idx);
	if (ret < 0) {
		LOG_ERR("Format '%s' %ux%u not supported",
		        VIDEO_FOURCC_TO_STR(requested_pixelformat),
		        fmt->width,
		        fmt->height);
		ret = -ENOTSUP;
		goto err;
	}

	/* Centering the window on the pixel array must not shift the Bayer order */
	if (fmt->width % ov5647_fmts[idx].width_step != 0 ||
	    fmt->height % ov5647_fmts[idx].height_step != 0) {
		LOG_ERR("Resolution %ux%u is not a multiple of %ux%u",
		        fmt->width,
		        fmt->height,
		        ov5647_fmts[idx].width_step,
		        ov5647_fmts[idx].height_step);
		ret = -EINVAL;
		goto err;
	}

	/* data->streaming is false and the sensor is left parked (running, LP-11) rather than in
	 * standby from ov5647_init() onward -- so without this, the writes below land on a
	 * running PLL and can drop the lanes out of the LP-11 state the park exists to create.
	 * Drop to standby first; ov5647_lane_park() below re-asserts running and re-parks, so this
	 * does not race or double-write against set_stream(), which set_fmt() cannot run under
	 * (the data->streaming guard above).
	 */
	ret = video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, 0);
	if (ret < 0) {
		goto err;
	}

	ret = video_modify_cci_reg(&cfg->i2c,
	                           OV5647_SC_PLL_CTRL0,
	                           OV5647_MIPI_BIT_MODE,
	                           idx == OV5647_FMT_SBGGR8 ? OV5647_MIPI_BIT_MODE_RAW8
	                                                    : OV5647_MIPI_BIT_MODE_RAW10);
	if (ret < 0) {
		goto err;
	}

	ret = ov5647_set_mode_regs(dev, fmt->width, fmt->height);
	if (ret < 0) {
		goto err;
	}

	/* ov5647_set_mode_regs() just wrote 0x3820/0x3821 as whole bytes, which wipes any
	 * previously-set flip ctrl bits -- re-apply both so ctrl and hardware stay in sync. Safe
	 * to call before ov5647_init_ctrls() has run (the very first ov5647_init() call): a
	 * zero-initialized ctrls->hflip.val/vflip.val reads as 0 (default, unflipped), which
	 * matches the mode bytes' own default bits[2:1] = 0 -- see AUTHORIZED LOCAL DIVERGENCE #3.
	 */
	ret = ov5647_set_ctrl_hflip(dev);
	if (ret < 0) {
		goto err;
	}

	ret = ov5647_set_ctrl_vflip(dev);
	if (ret < 0) {
		goto err;
	}

	data->fmt = *fmt;

	/* The reachable frame rates depend on the height that was just programmed */
	ret = ov5647_set_frmival(dev, &frmival);
	if (ret < 0) {
		goto err;
	}

	ret = ov5647_lane_park(dev);
	if (ret < 0) {
		goto err;
	}

	/* Report the EFFECTIVE fourcc back, matching ov5647_get_fmt() below -- data->fmt just
	 * stored above is the BASE fourcc video_format_caps_index() matched; hflip/vflip (the flip
	 * ctrls ov5647_set_ctrl_hflip()/vflip() just re-applied to hardware, unchanged by this
	 * function) may shift that away from what the caller's fmt->pixelformat still holds.
	 */
	fmt->pixelformat = ov5647_bayer_pixfmt(data->fmt.pixelformat, hflip, vflip);

	return 0;

err:
	/* Every failure path above runs after fmt->pixelformat was remapped to a BASE fourcc
	 * (the round-5 accept-a-flip-shifted-fourcc block above) -- restore what the CALLER
	 * actually passed in, on every one of them, not just the first (issue #2248 fix-up
	 * round 6): a caller inspecting fmt->pixelformat after a failed set_fmt() must see its
	 * own request back, never the driver's internal base-fourcc remap.
	 */
	fmt->pixelformat = requested_pixelformat;
	return ret;
}

/*
 * The flip ctrls (VIDEO_CID_HFLIP/VFLIP) shift the Bayer colour order, not just the image
 * orientation -- issue #2248, round 4: mirroring an even-sized Bayer array along an axis swaps
 * that axis's colour pairing (e.g. BGGR horizontally mirrored reads GBRG).
 * ov5647_set_ctrl_hflip()/vflip() above only ever touch 0x3821/0x3820's mirror bits, so the
 * sensor DOES shift order on a flip; this function makes ov5647_get_fmt() report that shift
 * instead of always reporting the mode's base (unflipped) pixelformat, which would tell demosaic
 * code the wrong colour order once either ctrl is set. NOT documentation-only, despite writing no
 * NEW registers: round 4 changed what get_format() REPORTS, and round 5 additionally changed what
 * set_format() ACCEPTS and RETURNS (ov5647_set_fmt() above now maps one of the flip-shifted
 * fourccs this function derives back to a base fourcc on input, and reports the shifted fourcc
 * back on output) -- both are API contract changes, not just a comment or a report-only add-on.
 *
 * Mapping translated from the RPi/OmniVision reference's ov5647_get_mbus_code() (rpi-6.6.y
 * drivers/media/i2c/ov5647.c): `codes[hflip | (vflip << 1)] = {SGBRG, SBGGR, SRGGB, SGRBG}`. That
 * reference's OWN V4L2_CID_HFLIP ctrl is the LOGICAL INVERSE of the 0x3821 mirror bit --
 * ov5647_s_ctrl() there writes `!ctrl->val` to the register (there's an in-built hflip in the
 * silicon, per that driver's own comment) -- while THIS driver's HFLIP ctrl bit IS the mirror bit
 * directly (ov5647_set_ctrl_hflip() above: `hflip.val != 0 ? OV5647_TC_REG21_MIRROR : 0`, no
 * inversion). VFLIP is direct (not inverted) in both drivers. Substituting rpi_hflip = !our_hflip
 * and rpi_vflip = our_vflip into the reference's table, in terms of OUR ctrl values: (0,0) ->
 * SBGGR, (1,0) -> SGBRG, (0,1) -> SGRBG, (1,1) -> SRGGB. (0,0) landing on SBGGR matches the
 * maintainer-confirmed default orientation (run 62) as a sanity check on this translation.
 *
 * Only the default (0,0) order has been bench/maintainer-verified as the correct rendered colour
 * order; the three flipped orders below are DERIVED from the reference mapping above, not
 * bench-checked on this module -- see docs/camera-shields.md.
 */
static uint32_t ov5647_bayer_pixfmt(uint32_t base_pixelformat, bool hflip, bool vflip)
{
	switch (base_pixelformat) {
	case VIDEO_PIX_FMT_SBGGR8:
		if (hflip && vflip) {
			return VIDEO_PIX_FMT_SRGGB8;
		} else if (hflip) {
			return VIDEO_PIX_FMT_SGBRG8;
		} else if (vflip) {
			return VIDEO_PIX_FMT_SGRBG8;
		}
		return VIDEO_PIX_FMT_SBGGR8;
	case VIDEO_PIX_FMT_SBGGR10P:
		if (hflip && vflip) {
			return VIDEO_PIX_FMT_SRGGB10P;
		} else if (hflip) {
			return VIDEO_PIX_FMT_SGBRG10P;
		} else if (vflip) {
			return VIDEO_PIX_FMT_SGRBG10P;
		}
		return VIDEO_PIX_FMT_SBGGR10P;
	default:
		return base_pixelformat;
	}
}

static int ov5647_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct ov5647_data *data = dev->data;

	*fmt             = data->fmt;
	fmt->pixelformat = ov5647_bayer_pixfmt(
	    data->fmt.pixelformat, data->ctrls.hflip.val != 0, data->ctrls.vflip.val != 0);

	return 0;
}

/*
 * ov5647_fmts[] (what this advertises) only lists the two BASE (unflipped) pixelformats --
 * set_fmt() ACCEPTS a flip-shifted fourcc too (issue #2248 fix-up round 5), but only the one the
 * CURRENT flip ctrls derive from a base entry here, by mapping it back to that base fourcc before
 * validating (see the comment in ov5647_set_fmt()); a flip-shifted fourcc that does not match the
 * current flip state is still rejected. The three derived flipped fourccs are therefore not
 * listed as independently settable formats here -- they are a side effect of the separate
 * VIDEO_CID_HFLIP/VFLIP ctrls, reported by ov5647_get_fmt() above, not a size/depth choice this
 * caps table itself offers.
 */
static int ov5647_get_caps(const struct device *dev, struct video_caps *caps)
{
	if (caps->type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	caps->format_caps = ov5647_fmts;

	return 0;
}

/*
 * Park the CSI-2 lanes at LP-11; see the OV5647_MIPI_CTRL00 comment above for why and the bench
 * evidence. Must be called with the sensor left running (MODE_SELECT_STREAMING set) -- parking
 * from software standby does not present LP-11.
 *
 * If any of the three writes after MODE_SELECT fails, the sensor is left running but only
 * partway through the park sequence -- streaming on the CSI bus in an undefined lane state. On
 * that path, make a best-effort drop back to standby before returning the original error; the
 * standby write's own result is not checked (there is already an error in flight to report, and
 * this is strictly better-effort than leaving the sensor running).
 */
static int ov5647_lane_park(const struct device *dev)
{
	const struct ov5647_config *cfg = dev->config;
	int                         ret;

	ret = video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, OV5647_MODE_SELECT_STREAMING);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c,
	                          OV5647_MIPI_CTRL00,
	                          OV5647_MIPI_CTRL00_CLOCK_LANE_GATE | OV5647_MIPI_CTRL00_BUS_IDLE |
	                              OV5647_MIPI_CTRL00_CLOCK_LANE_DISABLE);
	if (ret < 0) {
		video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, 0);
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_FRAME_OFF_NUM, OV5647_FRAME_OFF_NUM_PARKED);
	if (ret < 0) {
		video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, 0);
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_PAD_OUT, OV5647_PAD_OUT_PARKED);
	if (ret < 0) {
		video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, 0);
		return ret;
	}

	return 0;
}

/* Forward declarations: ov5647_set_stream(true) below re-asserts both AEC/AGC-manual bits
 * (0x3503, OV5647_MANUAL_CTRL) from the ctrls cache immediately before the sensor is allowed to
 * stream -- #2277 (bench run 247), see that call site's own comment. Both setters are defined
 * further down, next to ov5647_set_ctrl() which normally dispatches to them.
 */
static int ov5647_set_ctrl_gain(const struct device *dev);
static int ov5647_set_ctrl_exposure(const struct device *dev);

static int ov5647_set_stream(const struct device *dev, bool on, enum video_buf_type type)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;
	int                         ret;

	if (type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	if (!on) {
		/* Re-park rather than only dropping to software standby, so a stopped stream still
		 * presents LP-11 and a later stream start can unpark it cleanly.
		 */
		ret = ov5647_lane_park(dev);
		if (ret < 0) {
			return ret;
		}

		data->streaming = false;

		return 0;
	}

	/* Deliberate continuous-clock choice: this writes only BUS_IDLE, clearing the park's
	 * CLOCK_LANE_GATE/CLOCK_LANE_DISABLE bits and leaving the MIPI clock lane running between
	 * frames, overriding a power-on default the driver previously never touched. Mainline
	 * additionally sets CLOCK_LANE_GATE | LINE_SYNC_ENABLE when the endpoint declares a
	 * non-continuous clock; a `clock-noncontinuous` endpoint property is not honoured here.
	 */
	ret = video_write_cci_reg(&cfg->i2c, OV5647_MIPI_CTRL00, OV5647_MIPI_CTRL00_BUS_IDLE);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_FRAME_OFF_NUM, OV5647_FRAME_OFF_NUM_STREAMING);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_PAD_OUT, OV5647_PAD_OUT_STREAMING);
	if (ret < 0) {
		return ret;
	}

	/*
	 * #2277 (bench run 247): re-assert both AEC/AGC-manual bits (0x3503, OV5647_MANUAL_CTRL)
	 * from the ctrls cache right here -- the LAST write this driver makes before
	 * MODE_SELECT_STREAMING below lets pixels flow. Bench evidence: isp_pico.c's
	 * isp_apply_ae_sensor_gate() correctly sets ctrls->exposure_auto/auto_gain to
	 * MANUAL/off and RMWs the bits into 0x3503 well before this point, yet a live
	 * readback during streaming found 0x3503 = 0x04 (VTS-manual only, AEC/AGC-manual
	 * both clear) with the sensor's own on-chip AEC pinning exposure at 32 lines --
	 * something between the gate and streaming clobbers those two bits. WHAT clobbers
	 * them is not pinned down (every write this driver itself makes to 0x3503 is
	 * already a read-modify-write of a single bit, never a whole-byte overwrite, once
	 * ov5647_init_regs[]'s own one-time boot write is past -- see that table's own
	 * comment); this re-assert is the defensive fix regardless of the exact source,
	 * matching the same "reapply right before streaming" shape as ov5647_set_fmt()'s
	 * own flip-ctrl re-apply after ov5647_set_mode_regs() above.
	 *
	 * Both setters are ctrls-cache-driven RMWs (video_modify_cci_reg(), see their own
	 * definitions below), so this is a safe no-op on the dev-default raw-capture path
	 * (e.g. aen-camera-firstlight) where exposure_auto/auto_gain are never moved off
	 * their AUTO defaults: it just re-writes the AUTO bits the sensor's own on-chip
	 * AEC/AGC is already supposed to have.
	 */
	ret = ov5647_set_ctrl_gain(dev);
	if (ret < 0) {
		return ret;
	}

	ret = ov5647_set_ctrl_exposure(dev);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, OV5647_MODE_SELECT_STREAMING);
	if (ret < 0) {
		return ret;
	}

	data->streaming = true;

	return 0;
}

static int ov5647_set_ctrl_gain(const struct device *dev)
{
	const struct ov5647_config *cfg   = dev->config;
	struct ov5647_data         *data  = dev->data;
	struct ov5647_ctrls        *ctrls = &data->ctrls;
	int                         ret;

	ret = video_modify_cci_reg(&cfg->i2c,
	                           OV5647_MANUAL_CTRL,
	                           OV5647_MANUAL_CTRL_AGC,
	                           ctrls->auto_gain.val != 0 ? 0 : OV5647_MANUAL_CTRL_AGC);
	if (ret < 0) {
		return ret;
	}

	if (ctrls->auto_gain.val != 0) {
		return 0;
	}

	return video_write_cci_reg(&cfg->i2c, OV5647_AGC_GAIN, ctrls->gain.val);
}

static int ov5647_set_ctrl_exposure(const struct device *dev)
{
	const struct ov5647_config *cfg   = dev->config;
	struct ov5647_data         *data  = dev->data;
	struct ov5647_ctrls        *ctrls = &data->ctrls;
	int                         ret;

	ret = video_modify_cci_reg(
	    &cfg->i2c,
	    OV5647_MANUAL_CTRL,
	    OV5647_MANUAL_CTRL_AEC,
	    ctrls->exposure_auto.val == VIDEO_EXPOSURE_MANUAL ? OV5647_MANUAL_CTRL_AEC : 0);
	if (ret < 0) {
		return ret;
	}

	if (ctrls->exposure_auto.val != VIDEO_EXPOSURE_MANUAL) {
		return 0;
	}

	return video_write_cci_reg(&cfg->i2c, OV5647_EXPOSURE, ctrls->exposure.val);
}

static int ov5647_set_ctrl_test_pattern(const struct device *dev)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;
	int32_t                     val  = data->ctrls.test_pattern.val;

	return video_write_cci_reg(
	    &cfg->i2c, OV5647_ISP_CTRL3D, val == 0 ? 0 : (OV5647_TEST_PATTERN_ENABLE | (val - 1)));
}

/* Also called from ov5647_set_fmt() to re-apply after ov5647_set_mode_regs() wipes the mode
 * byte -- issue #2248 fix-up round 3, see the forward declaration above ov5647_set_fmt().
 */
static int ov5647_set_ctrl_hflip(const struct device *dev)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;

	return video_modify_cci_reg(&cfg->i2c,
	                            OV5647_TIMING_TC_REG21,
	                            OV5647_TC_REG21_MIRROR,
	                            data->ctrls.hflip.val != 0 ? OV5647_TC_REG21_MIRROR : 0);
}

static int ov5647_set_ctrl_vflip(const struct device *dev)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;

	return video_modify_cci_reg(&cfg->i2c,
	                            OV5647_TIMING_TC_REG20,
	                            OV5647_TC_REG20_VFLIP,
	                            data->ctrls.vflip.val != 0 ? OV5647_TC_REG20_VFLIP : 0);
}

static int ov5647_set_ctrl(const struct device *dev, uint32_t cid)
{
	switch (cid) {
	case VIDEO_CID_AUTOGAIN:
		return ov5647_set_ctrl_gain(dev);
	case VIDEO_CID_EXPOSURE_AUTO:
	case VIDEO_CID_EXPOSURE:
		return ov5647_set_ctrl_exposure(dev);
	case VIDEO_CID_HFLIP:
		return ov5647_set_ctrl_hflip(dev);
	case VIDEO_CID_VFLIP:
		return ov5647_set_ctrl_vflip(dev);
	case VIDEO_CID_TEST_PATTERN:
		return ov5647_set_ctrl_test_pattern(dev);
	default:
		return -ENOTSUP;
	}
}

static int ov5647_get_volatile_ctrl(const struct device *dev, uint32_t cid)
{
	const struct ov5647_config *cfg  = dev->config;
	struct ov5647_data         *data = dev->data;
	uint32_t                    gain;
	int                         ret;

	if (cid != VIDEO_CID_AUTOGAIN) {
		return -ENOTSUP;
	}

	ret = video_read_cci_reg(&cfg->i2c, OV5647_AGC_GAIN, &gain);
	if (ret < 0) {
		return ret;
	}

	data->ctrls.gain.val = gain & OV5647_AGC_GAIN_MAX;

	return 0;
}

static DEVICE_API(video, ov5647_driver_api) = {
	.set_format        = ov5647_set_fmt,
	.get_format        = ov5647_get_fmt,
	.get_caps          = ov5647_get_caps,
	.set_stream        = ov5647_set_stream,
	.set_ctrl          = ov5647_set_ctrl,
	.get_volatile_ctrl = ov5647_get_volatile_ctrl,
	.set_frmival       = ov5647_set_frmival,
	.get_frmival       = ov5647_get_frmival,
	.enum_frmival      = ov5647_enum_frmival,
};

static const char *const ov5647_exposure_auto_menu[] = {
	"Auto Mode",
	"Manual Mode",
	NULL,
};

static const char *const ov5647_test_pattern_menu[] = {
	"Off", "Color bar", "Color square", "Random data", NULL,
};

static int ov5647_init_ctrls(const struct device *dev)
{
	const struct ov5647_config *cfg   = dev->config;
	struct ov5647_data         *data  = dev->data;
	struct ov5647_ctrls        *ctrls = &data->ctrls;
	int                         ret;

	ret = video_init_ctrl(&ctrls->auto_gain,
	                      dev,
	                      VIDEO_CID_AUTOGAIN,
	                      (struct video_ctrl_range){ .min = 0, .max = 1, .step = 1, .def = 1 });
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(
	    &ctrls->gain,
	    dev,
	    VIDEO_CID_ANALOGUE_GAIN,
	    (struct video_ctrl_range){ .min = 0, .max = OV5647_AGC_GAIN_MAX, .step = 1, .def = 0 });
	if (ret < 0) {
		return ret;
	}

	ret = video_auto_cluster_ctrl(&ctrls->auto_gain, 2, true);
	if (ret < 0) {
		return ret;
	}

	ret = video_init_menu_ctrl(&ctrls->exposure_auto,
	                           dev,
	                           VIDEO_CID_EXPOSURE_AUTO,
	                           VIDEO_EXPOSURE_AUTO,
	                           ov5647_exposure_auto_menu);
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(
	    &ctrls->exposure,
	    dev,
	    VIDEO_CID_EXPOSURE,
	    (struct video_ctrl_range){
	        .min = 0, .max = OV5647_EXPOSURE_MAX, .step = 1, .def = OV5647_EXPOSURE_DEFAULT });
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->hflip,
	                      dev,
	                      VIDEO_CID_HFLIP,
	                      (struct video_ctrl_range){ .min = 0, .max = 1, .step = 1, .def = 0 });
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->vflip,
	                      dev,
	                      VIDEO_CID_VFLIP,
	                      (struct video_ctrl_range){ .min = 0, .max = 1, .step = 1, .def = 0 });
	if (ret < 0) {
		return ret;
	}

	ret = video_init_menu_ctrl(
	    &ctrls->test_pattern, dev, VIDEO_CID_TEST_PATTERN, 0, ov5647_test_pattern_menu);
	if (ret < 0) {
		return ret;
	}

	return video_init_ctrl(&ctrls->pixel_rate,
	                       dev,
	                       VIDEO_CID_PIXEL_RATE,
	                       (struct video_ctrl_range){ .min64  = cfg->pixel_rate,
	                                                  .max64  = cfg->pixel_rate,
	                                                  .step64 = 1,
	                                                  .def64  = cfg->pixel_rate });
}

static int ov5647_init(const struct device *dev)
{
	const struct ov5647_config *cfg = dev->config;
	/*
	 * PRE-MERGE REGRESSION, caught before merge (issue #2248, bench run 62): the run-61 PLL
	 * retarget (AUTHORIZED LOCAL DIVERGENCE #3, OV5647_PLL_MULT 105 -> 70) lowered
	 * cfg->pixel_rate from 87500000 to 58333333 within this same unreleased PR chain. That
	 * silently broke booting into the full-resolution crop (OV5647_FULL_WIDTH/HEIGHT, this
	 * driver's boot format at the time): the driver's own default data->frmrate (15) became
	 * unreachable at that height -- ov5647_frmrate_to_vts(dev, OV5647_HTS_CROP, 15) now gives
	 * VTS 1440, below OV5647_FULL_HEIGHT (1944) + OV5647_VBLANK_MIN (24) = 1968, where the
	 * PRE-run-61 pixel_rate (87500000) had made it reachable -- and Zephyr's
	 * video_closest_frmival() stops enumerating at the FIRST unreachable (sorted-ascending)
	 * rate, so it never even looked past 10 fps. This was never released: bench run 62 caught
	 * it before merge, in the same fix-up pass that also moved this boot format to 640x480 (a
	 * genuine, independent improvement -- 640x480 is this driver's real, bench-proven default
	 * mode, see AUTHORIZED LOCAL DIVERGENCE #3), where 15 fps IS reachable (VTS 2099 = 0x0833
	 * >= 480 + 24). A caller that explicitly switches to a full-resolution crop still gets a
	 * valid, if lower, rate -- HTS_CROP=2700 genuinely cannot sustain 15 fps at 1944 lines;
	 * that is a real PLL/line-time limit, not a bug, and video_closest_frmival() correctly
	 * falls back to the next reachable entry in that case. NOTE: booting into a reachable
	 * format alone does not keep every LATER format change at 15 fps -- see
	 * data->requested_frmival's declaration and ov5647_set_fmt() for the separate fix-up
	 * round 3 FIXED bug that does (the rate used to stick at whatever a PRIOR mode had
	 * clamped it to).
	 */
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = OV5647_MODE_640X480_WIDTH,
		.height      = OV5647_MODE_640X480_HEIGHT,
	};
	uint32_t chip_id;
	int      ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(pwdn_gpios)
	if (cfg->pwdn_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->pwdn_gpio)) {
			LOG_ERR("Device %s is not ready", cfg->pwdn_gpio.port->name);
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->pwdn_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			return ret;
		}

		/* Datasheet section 2.5: the supplies must be stable before PWDN is released */
		k_sleep(K_MSEC(5));

		gpio_pin_set_dt(&cfg->pwdn_gpio, 0);

		/* Datasheet section 2.5: SCCB is only accessible 20 ms after PWDN goes low */
		k_sleep(K_MSEC(20));
	}
#endif

	ret = video_write_cci_reg(&cfg->i2c, OV5647_SOFTWARE_RESET, OV5647_SOFTWARE_RESET_RESET);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(5));

	ret = video_read_cci_reg(&cfg->i2c, OV5647_CHIP_ID_REG, &chip_id);
	if (ret < 0) {
		return ret;
	}

	if (chip_id != OV5647_CHIP_ID) {
		LOG_ERR("Wrong chip ID 0x%04x instead of 0x%04x", chip_id, OV5647_CHIP_ID);
		return -ENODEV;
	}

	/* ov5647_init_regs[] now routes the pixel stream to the MIPI transmitter itself
	 * (OV5647_SC_MIPI_SC_CTRL = 0x44, run 61's literal reference byte) -- superseded the
	 * targeted PHY_PD_MIPI/PHY_PD_LPRX/MIPI_EN bitfield modify this call used to make here.
	 */
	ret = video_write_cci_multiregs(&cfg->i2c, ov5647_init_regs, ARRAY_SIZE(ov5647_init_regs));
	if (ret < 0) {
		return ret;
	}

	/* #2277: set the VTS-manual bit as its own read-modify-write, not a table entry -- see
	 * ov5647_init_regs[]'s own comment just above OV5647_VTS_DIFF for why. The register is
	 * still at its post-reset 0 here (ov5647_init_ctrls(), below, hasn't run yet -- nothing
	 * has touched the AEC/AGC-manual bits this early), so this is a same-result RMW, not a
	 * behaviour change at boot.
	 */
	ret = video_modify_cci_reg(
	    &cfg->i2c, OV5647_MANUAL_CTRL, OV5647_MANUAL_CTRL_VTS, OV5647_MANUAL_CTRL_VTS);
	if (ret < 0) {
		return ret;
	}

	ret = ov5647_set_fmt(dev, &fmt);
	if (ret < 0) {
		return ret;
	}

	ret = ov5647_init_ctrls(dev);
	if (ret < 0) {
		return ret;
	}

	/* Leave the sensor parked at LP-11 rather than in bare standby, so a CSI-2 receiver that
	 * waits for Stop-state can already see the lanes before the first ov5647_set_stream(true).
	 */
	return ov5647_lane_park(dev);
}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(pwdn_gpios)
#define OV5647_GET_PWDN_GPIO(n) .pwdn_gpio = GPIO_DT_SPEC_INST_GET_OR(n, pwdn_gpios, { 0 }),
#else
#define OV5647_GET_PWDN_GPIO(n)
#endif

#define OV5647_EP(n)        DT_CHILD(DT_INST_CHILD(n, port), endpoint)
#define OV5647_INPUT_CLK(n) DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency)

#define OV5647_INIT(n) \
	BUILD_ASSERT(DT_PROP_OR(OV5647_EP(n), bus_type, VIDEO_BUS_TYPE_CSI2_DPHY) == \
	                 VIDEO_BUS_TYPE_CSI2_DPHY, \
	             "Only the MIPI CSI-2 D-PHY interface is supported"); \
	BUILD_ASSERT(DT_PROP_LEN_OR(OV5647_EP(n), data_lanes, 2) == 2, \
	             "Only the two data lanes mode is supported"); \
	BUILD_ASSERT(IN_RANGE(OV5647_INPUT_CLK(n), OV5647_INPUT_CLK_MIN, OV5647_INPUT_CLK_MAX), \
	             "XVCLK must be between 6 MHz and 27 MHz"); \
\
	static struct ov5647_data ov5647_data_##n = { \
		.frmrate           = 15, \
		.requested_frmival = { .numerator = 1, .denominator = 15 }, \
	}; \
\
	static const struct ov5647_config ov5647_cfg_##n = { \
		.i2c                               = I2C_DT_SPEC_INST_GET(n), \
		OV5647_GET_PWDN_GPIO(n).pixel_rate = OV5647_PIXEL_RATE(OV5647_INPUT_CLK(n)), \
	}; \
\
	DEVICE_DT_INST_DEFINE(n, \
	                      &ov5647_init, \
	                      NULL, \
	                      &ov5647_data_##n, \
	                      &ov5647_cfg_##n, \
	                      POST_KERNEL, \
	                      CONFIG_VIDEO_INIT_PRIORITY, \
	                      &ov5647_driver_api); \
\
	VIDEO_DEVICE_DEFINE(ov5647_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(OV5647_INIT)
