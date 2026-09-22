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
 * includes all three explicitly) and apart from the one AUTHORIZED LOCAL
 * DIVERGENCE below. See zephyr/CMakeLists.txt / zephyr/kconfigs/ for the
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
 * The values match mainline, which is the reference, so shipping them
 * together is right, but do not read this as proof the PLL pair alone is
 * the whole fix. 0x4837 (PCLK period) is left at its power-on 0x15
 * (mainline writes 0x19 for this PLL); on the safe side at 437.5 Mbps and
 * not changed in the working run.
 *
 * AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248, bench runs 56/60): the driver's 640x480 was a
 * 648x488 1:1 CENTRE CROP -- roughly 25% of the array width, a heavy telephoto crop -- not the
 * full-array subsampled+binned 640x480 both Alif's own validated table for this exact silicon
 * (alif-dfp-ref components/Source/OV5647_camera_sensor.c) and mainline Linux use. Run 60's
 * full-FOV register set (window 0x3800..0x3807 = 0x0010,0x0000..0x0a2f,0x079f; output size
 * 0x3808/0x3809 = 640, 0x380a/0x380b = 480; subsample 0x3814/0x3815 = 0x35; binning
 * 0x3821/0x3820 = 0x07/0x41; binned-mode analog 0x3612/0x3618/0x3708/0x3709 =
 * 0x59/0x00/0x64/0x52), written in software standby after ov5647_set_window() and before the
 * lane park, streamed cleanly with a real, recognisable image. HTS stays the driver's 2700
 * (bench-proven here; Alif's 1852 is untested on this board); the PLL stays as DIVERGENCE #2
 * programs it. Now the 640x480 branch of ov5647_set_mode_regs() below.
 *
 * ORDERING TRAP (bench run 54, MUST NOT be reintroduced): binning is only coherent with the
 * full-array window -- 0x3814 = 0x35 is a ~/4 decimation (2592/4 = 648, 1944/4 = 486). Run 54
 * applied the binning registers and then let ov5647_set_window() rewrite the window to the
 * crop; the sensor emitted short lines against a 640-pixel frame declaration and the CSI host
 * raised "Fatal Interrupt due to mismatch of Frame Start and Frame End" on VC0 44 times in 2 s
 * and delivered nothing. So the window, output size, subsample and binning registers are always
 * written as ONE coherent set per mode -- never binning left on with a crop window. Every OTHER
 * requested size therefore ALSO explicitly writes the 1:1 values (0x3814/0x3815 = 0x11,
 * 0x3820/0x3821 = 0x40/0x00) plus mainline's full-resolution 1:1 analog values
 * (0x3612/0x3618/0x3708/0x3709 = 0x5b/0x04/0x64/0x12, cited from mainline, BENCH-UNVERIFIED on
 * this module) right after ov5647_set_window(), so switching 640x480 -> another size -> back
 * never leaves binning armed on a stale crop window.
 *
 * Common analog bias / BLC / AEC registers (bench run 58, phases PC/PD/PF, 0 register
 * read-back mismatches, clean streaming): mainline writes these in ov5647_common_regs[] for
 * every mode, and this driver never had. Now appended to ov5647_init_regs[] -- see that array
 * for the values. Deliberately NOT added: the ISP block enables (0x5000..0x5003/0x5a00) -- run
 * 58 phase PB, the only phase that added them, is the phase that threw CSI "incorrect frame
 * sequence" fatals at stream-on. The ISP is left at its power-on state.
 *
 * Also from the same bench pass: OV5647_EXPOSURE_DEFAULT was 0x20 (2 lines in 1/16-line units)
 * -- effectively a closed shutter for any manual-exposure user. Now 0x0FFF (~256 lines),
 * matching the order of magnitude of Alif's own shipped default (~0x000FFF) for this silicon.
 * At the default 15 fps VTS in a dark lab the AEC railed at both its limits (502 lines, 2x the
 * 0x3a0b band step; gain at the 0x3a19 ceiling) and a real image needed ~16000 lines (0.49 s) --
 * a SCENE limitation, not a driver bug; the AEC limits above are left unchanged.
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
 * derivation), AND DIVERGENCE #3 (the full-FOV binned 640x480 mode, the
 * common analog/BLC/AEC init, and the corrected OV5647_EXPOSURE_DEFAULT) --
 * deleting this file without checking all three silently reintroduces one
 * or more bugs. Do NOT otherwise maintain divergent local
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

#define OV5647_CHIP_ID			0x5647

#define OV5647_FULL_WIDTH		2592
#define OV5647_FULL_HEIGHT		1944

/* Position of the first output pixel in the pixel array, from the power-on window */
#define OV5647_X_ADDR_START		12
#define OV5647_Y_ADDR_START		4

/*
 * The read-out window is wider and taller than the output window by this many pixels, which the
 * ISP crops back off using the offsets at 0x3810..0x3813 left at their power-on values.
 */
#define OV5647_WINDOW_MARGIN		8

/* Line length, kept at its power-on value whatever the output size is */
#define OV5647_HTS			2700
/* Power-on frame length minus the full output height */
#define OV5647_VBLANK_MIN		24

/*
 * PLL constants for the RAW10 (10bpp) mode ov5647_init() selects -- see AUTHORIZED LOCAL
 * DIVERGENCE #2 in the file header for the bench evidence and ov5647_init_regs[] below for where
 * these actually get written to the sensor. Shared with that register table so the programmed PLL
 * and the declared pixel_rate can never drift apart: VCO = XVCLK / OV5647_PLL_PREDIV *
 * OV5647_PLL_MULT; lane bit rate = VCO / OV5647_PLL_SYS_DIV. At 25 MHz XVCLK: VCO = 875 MHz, lane
 * bit rate = 437.5 Mbps/lane, matching mainline Linux's own declared link_freq (218750000, DDR
 * half-rate) for this PLL.
 */
#define OV5647_PLL_PREDIV		3
#define OV5647_PLL_MULT			105
#define OV5647_PLL_SYS_DIV		2

/*
 * pixel_rate = lane bit rate * lanes / bpp = (XVCLK / sysdiv * mult / prediv) * 2 / 10 (divide by
 * sysdiv first, not prediv, to stay exact and inside int32 range for every XVCLK in
 * [OV5647_INPUT_CLK_MIN, OV5647_INPUT_CLK_MAX]). At 25 MHz XVCLK: 87500000, matching mainline's
 * own declared pixel_rate for this PLL. Feeds BOTH VIDEO_CID_PIXEL_RATE and
 * ov5647_frmrate_to_vts() below -- it sets the programmed TIMING_VTS too (87500000/(2700*15) =
 * 2160 instead of the previous, power-on-PLL-derived 2058; ov5647_enum_frmival() only rejects a
 * frame rate whose VTS would undercut the read-out height, so the larger, correct VTS just changes
 * which of the fixed ov5647_framerates[] entries are reachable, which is expected).
 *
 * A previous attempt to "correct" this macro alone, in isolation, to a PLL-derived 175000000
 * (issue #2248) was reverted -- not because the number was wrong, but because nothing programmed
 * the PLL to match it: the reverted number was actually right about the power-on link rate (see
 * DIVERGENCE #2), and "it doubled the programmed VTS" was the correct consequence of a doubled
 * pixel clock, not evidence against it. The defect was that the PLL was never WRITTEN at all; see
 * docs/camera-shields.md.
 *
 * NOT bench-verified for 8-bit: cfg->pixel_rate below is a single value computed once from DT at
 * OV5647_INIT() time, not re-derived per selected format. ov5647_set_fmt() does reprogram 0x3034's
 * OV5647_MIPI_BIT_MODE field per format (RAW8 vs RAW10), but nothing here recomputes pixel_rate to
 * match, so selecting VIDEO_PIX_FMT_SBGGR8 (8bpp) still reports the RAW10 value. Video_get_csi_
 * link_freq()'s PIXEL_RATE fallback (zephyr/drivers/video/video_common.c) computes link_freq =
 * pixel_rate * bpp / (2*lanes); at the same 875 MHz VCO, 8bpp would need pixel_rate = 437.5e6 * 2 /
 * 8 = 109375000, not 87500000, to feed that formula correctly. Only RAW10 is bench-proven (run
 * 52) -- flagging this gap, not shipping an unverified 8-bit number.
 */
#define OV5647_PIXEL_RATE(clk) \
	((clk) / OV5647_PLL_SYS_DIV * OV5647_PLL_MULT / OV5647_PLL_PREDIV * 2 / OV5647_MIPI_BIT_MODE_RAW10)
#define OV5647_INPUT_CLK_MIN		MHZ(6)
#define OV5647_INPUT_CLK_MAX		MHZ(27)

#define OV5647_REG8(addr)		((addr) | VIDEO_REG_ADDR16_DATA8)
#define OV5647_REG16(addr)		((addr) | VIDEO_REG_ADDR16_DATA16_BE)
#define OV5647_REG24(addr)		((addr) | VIDEO_REG_ADDR16_DATA24_BE)

#define OV5647_MODE_SELECT		OV5647_REG8(0x0100)
#define OV5647_MODE_SELECT_STREAMING	BIT(0)
#define OV5647_SOFTWARE_RESET		OV5647_REG8(0x0103)
#define OV5647_SOFTWARE_RESET_RESET	BIT(0)
#define OV5647_CHIP_ID_REG		OV5647_REG16(0x300a)
#define OV5647_SC_MIPI_PHY		OV5647_REG8(0x3016)
#define OV5647_MIPI_PAD_ENABLE		BIT(3)
#define OV5647_SC_MIPI_SC_CTRL		OV5647_REG8(0x3018)
#define OV5647_PHY_PD_MIPI		BIT(4)
#define OV5647_PHY_PD_LPRX		BIT(3)
#define OV5647_MIPI_EN			BIT(2)
#define OV5647_SC_PLL_CTRL0		OV5647_REG8(0x3034)
#define OV5647_MIPI_BIT_MODE		GENMASK(3, 0)
#define OV5647_EXPOSURE			OV5647_REG24(0x3500)
#define OV5647_EXPOSURE_MAX		GENMASK(19, 0)
/*
 * AUTHORIZED LOCAL DIVERGENCE #3: the upstream default, 0x20 (2 lines in 1/16-line units), is
 * effectively a closed shutter for any manual-exposure user. 0x0FFF (~256 lines) matches the
 * order of magnitude of Alif's own shipped default (~0x000FFF) for this exact silicon.
 */
#define OV5647_EXPOSURE_DEFAULT		0x0FFF
#define OV5647_MANUAL_CTRL		OV5647_REG8(0x3503)
#define OV5647_MANUAL_CTRL_VTS		BIT(2)
#define OV5647_MANUAL_CTRL_AGC		BIT(1)
#define OV5647_MANUAL_CTRL_AEC		BIT(0)
#define OV5647_AGC_GAIN			OV5647_REG16(0x350a)
#define OV5647_AGC_GAIN_MAX		GENMASK(9, 0)
#define OV5647_VTS_DIFF			OV5647_REG16(0x350c)
#define OV5647_TIMING_X_ADDR_START	OV5647_REG16(0x3800)
#define OV5647_TIMING_Y_ADDR_START	OV5647_REG16(0x3802)
#define OV5647_TIMING_X_ADDR_END	OV5647_REG16(0x3804)
#define OV5647_TIMING_Y_ADDR_END	OV5647_REG16(0x3806)
#define OV5647_TIMING_X_OUTPUT_SIZE	OV5647_REG16(0x3808)
#define OV5647_TIMING_Y_OUTPUT_SIZE	OV5647_REG16(0x380a)
#define OV5647_TIMING_HTS_REG		OV5647_REG16(0x380c)
#define OV5647_TIMING_VTS_REG		OV5647_REG16(0x380e)
#define OV5647_TIMING_TC_REG20		OV5647_REG8(0x3820)
#define OV5647_TC_REG20_VFLIP		(BIT(2) | BIT(1))
#define OV5647_TIMING_TC_REG21		OV5647_REG8(0x3821)
#define OV5647_TC_REG21_MIRROR		(BIT(2) | BIT(1))

/*
 * Subsample (0x3814/0x3815) and binning-enable bits of 0x3820/0x3821 (AUTHORIZED LOCAL
 * DIVERGENCE #3, issue #2248, bench runs 56/58/60) -- see the file header for the full
 * write-up. ov5647_set_mode_regs() below writes the whole window/output/subsample/binning set
 * as ONE coherent block per mode: binning is only coherent with the full-array window (0x3814 =
 * 0x35 is a ~/4 decimation), and bench run 54 proved leaving it armed on a crop window throws
 * CSI-2 frame-start/frame-end mismatches with no image delivered -- see the ORDERING TRAP note
 * in the file header. The base 0x3820/0x3821 values below deliberately leave the mirror/flip
 * bits (OV5647_TC_REG20_VFLIP / OV5647_TC_REG21_MIRROR) at 0 -- ov5647_set_ctrl()'s HFLIP/VFLIP
 * handlers read-modify-write only those two bits, so a format change resets any previously
 * requested mirror/flip and a caller must reapply VIDEO_CID_HFLIP/VFLIP after set_format().
 */
#define OV5647_TIMING_X_INC		OV5647_REG8(0x3814)
#define OV5647_TIMING_Y_INC		OV5647_REG8(0x3815)
#define OV5647_SUBSAMPLE_1TO1		0x11
#define OV5647_SUBSAMPLE_BINNED		0x35
#define OV5647_TC_REG20_1TO1		0x40
#define OV5647_TC_REG20_BINNED		0x41
#define OV5647_TC_REG21_1TO1		0x00
#define OV5647_TC_REG21_BINNED		0x07

/*
 * Analog registers that must track the subsample/binning mode above (same divergence, same
 * bench runs). 1:1 values are mainline Linux's full-resolution constants, cited but
 * BENCH-UNVERIFIED on this module; binned values are run-60's bench-proven binned-mode set.
 */
#define OV5647_ANALOG_CTRL12		OV5647_REG8(0x3612)
#define OV5647_ANALOG_CTRL12_1TO1	0x5b
#define OV5647_ANALOG_CTRL12_BINNED	0x59
#define OV5647_ANALOG_CTRL18		OV5647_REG8(0x3618)
#define OV5647_ANALOG_CTRL18_1TO1	0x04
#define OV5647_ANALOG_CTRL18_BINNED	0x00
#define OV5647_SENSOR_CTRL08		OV5647_REG8(0x3708)
#define OV5647_SENSOR_CTRL08_1TO1	0x64
#define OV5647_SENSOR_CTRL08_BINNED	0x64
#define OV5647_SENSOR_CTRL09		OV5647_REG8(0x3709)
#define OV5647_SENSOR_CTRL09_1TO1	0x12
#define OV5647_SENSOR_CTRL09_BINNED	0x52

/*
 * Full-array window read out by the 640x480 binned full-FOV mode (run 60) -- literal
 * bench-measured register values, NOT derived from OV5647_X_ADDR_START/OV5647_FULL_WIDTH like
 * ov5647_set_window()'s crop path below.
 */
#define OV5647_FULLFOV_X_ADDR_START	16
#define OV5647_FULLFOV_Y_ADDR_START	0
#define OV5647_FULLFOV_X_ADDR_END	0x0a2f
#define OV5647_FULLFOV_Y_ADDR_END	0x079f
#define OV5647_MODE_640X480_WIDTH	640
#define OV5647_MODE_640X480_HEIGHT	480

#define OV5647_ISP_CTRL3D		OV5647_REG8(0x503d)
#define OV5647_TEST_PATTERN_ENABLE	BIT(7)

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
#define OV5647_SC_PLL_CTRL1		OV5647_REG8(0x3035)
#define OV5647_SC_PLL_MULTIPLIER	OV5647_REG8(0x3036)
#define OV5647_SC_PLL_CTRL3		OV5647_REG8(0x3037)
/*
 * 0x303c and 0x3106 are not documented in the datasheet register map available to us (same
 * situation as OV5647_PAD_OUT / 0x300d below); bench-confirmed values only, part of the same
 * run-52 write set.
 */
#define OV5647_SC_PLL_CTRL_RSVD_303C	OV5647_REG8(0x303c)
#define OV5647_SC_CLKRST_RSVD_3106	OV5647_REG8(0x3106)

#define OV5647_IO_PAD_CTRL0		OV5647_REG8(0x3017)
#define OV5647_IO_PAD_CTRL0_PGM_LPTX	GENMASK(5, 4) /* LP TX (data/clock lane) drive strength */
#define OV5647_IO_PAD_CTRL0_PGM_VCM	GENMASK(7, 6) /* common-mode voltage drive strength */
/* 0x301c/0x301d: undocumented, same as 0x303c/0x3106 above */
#define OV5647_IO_PAD_CTRL1		OV5647_REG8(0x301c)
#define OV5647_IO_PAD_CTRL2		OV5647_REG8(0x301d)

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
#define OV5647_MIPI_CTRL00			OV5647_REG8(0x4800)
#define OV5647_MIPI_CTRL00_CLOCK_LANE_GATE	BIT(5)
#define OV5647_MIPI_CTRL00_BUS_IDLE		BIT(2)
#define OV5647_MIPI_CTRL00_CLOCK_LANE_DISABLE	BIT(0)
#define OV5647_FRAME_OFF_NUM			OV5647_REG8(0x4202)
#define OV5647_FRAME_OFF_NUM_PARKED		0x0f
#define OV5647_FRAME_OFF_NUM_STREAMING		0x00
/*
 * Address 0x300d is not documented in the datasheet register map available to us; these are the
 * bench-confirmed values needed alongside MIPI_CTRL00 and FRAME_OFF_NUM to complete the
 * park/unpark sequence. Mainline names this register OV5640_REG_PAD_OUT -- matching that name here
 * makes re-applying this fix upstream (see the file header's retirement note) mechanical.
 */
#define OV5647_PAD_OUT				OV5647_REG8(0x300d)
#define OV5647_PAD_OUT_PARKED			0x01
#define OV5647_PAD_OUT_STREAMING		0x00

/*
 * Datasheet table 7-1 describes the 0x3034 bit mode field as 0 for 8-bit and 1 for 10-bit, which
 * does not match its own 0x1A power-on value for this 10-bit sensor: the field holds the number of
 * bits, as it does on the rest of the family.
 */
#define OV5647_MIPI_BIT_MODE_RAW8	8
#define OV5647_MIPI_BIT_MODE_RAW10	10

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
	uint32_t frmrate;
	/* Tracks the APPLICATION's streaming request (set_stream), not OV5647_MODE_SELECT, which
	 * ov5647_lane_park() also drives while parked/unparked and which this field does not mirror.
	 */
	bool streaming;
};

/*
 * Common analog bias / BLC / AEC init (AUTHORIZED LOCAL DIVERGENCE #3, issue #2248, bench run
 * 58 phases PC/PD/PF -- 0 register read-back mismatches, clean streaming). Mainline writes these
 * in ov5647_common_regs[] for every mode; this driver never had. None of these addresses is
 * documented in the datasheet register map available to us, so they keep the file's existing
 * RSVD_<addr> naming (see OV5647_SC_PLL_CTRL_RSVD_303C above) rather than a guessed semantic
 * name -- bench-confirmed values only. Deliberately NOT added: the ISP block enables
 * (0x5000..0x5003/0x5a00) -- run 58 phase PB, the only phase that added them, is the phase that
 * threw CSI "incorrect frame sequence" fatals at stream-on. The ISP is left at its power-on
 * state.
 */
#define OV5647_ANALOG_RSVD_370C	OV5647_REG8(0x370c)
#define OV5647_ANALOG_RSVD_3630	OV5647_REG8(0x3630)
#define OV5647_ANALOG_RSVD_3632	OV5647_REG8(0x3632)
#define OV5647_ANALOG_RSVD_3633	OV5647_REG8(0x3633)
#define OV5647_ANALOG_RSVD_3634	OV5647_REG8(0x3634)
#define OV5647_ANALOG_RSVD_3620	OV5647_REG8(0x3620)
#define OV5647_ANALOG_RSVD_3621	OV5647_REG8(0x3621)
#define OV5647_ANALOG_RSVD_3600	OV5647_REG8(0x3600)
#define OV5647_ANALOG_RSVD_3704	OV5647_REG8(0x3704)
#define OV5647_ANALOG_RSVD_3703	OV5647_REG8(0x3703)
#define OV5647_ANALOG_RSVD_3715	OV5647_REG8(0x3715)
#define OV5647_ANALOG_RSVD_3717	OV5647_REG8(0x3717)
#define OV5647_ANALOG_RSVD_3731	OV5647_REG8(0x3731)
#define OV5647_ANALOG_RSVD_370B	OV5647_REG8(0x370b)
#define OV5647_ANALOG_RSVD_3705	OV5647_REG8(0x3705)
#define OV5647_ANALOG_RSVD_3F05	OV5647_REG8(0x3f05)
#define OV5647_ANALOG_RSVD_3F06	OV5647_REG8(0x3f06)
#define OV5647_ANALOG_RSVD_3F01	OV5647_REG8(0x3f01)
#define OV5647_ANALOG_RSVD_3C01	OV5647_REG8(0x3c01)
#define OV5647_ANALOG_RSVD_3B07	OV5647_REG8(0x3b07)
#define OV5647_ANALOG_RSVD_3636	OV5647_REG8(0x3636)
#define OV5647_ANALOG_RSVD_3827	OV5647_REG8(0x3827)
#define OV5647_BLC_RSVD_4001		OV5647_REG8(0x4001)
#define OV5647_BLC_RSVD_4004		OV5647_REG8(0x4004)
#define OV5647_BLC_RSVD_4000		OV5647_REG8(0x4000)
#define OV5647_BLC_RSVD_4050		OV5647_REG8(0x4050)
#define OV5647_BLC_RSVD_4051		OV5647_REG8(0x4051)
#define OV5647_AEC_RSVD_3A18		OV5647_REG8(0x3a18)
#define OV5647_AEC_RSVD_3A19		OV5647_REG8(0x3a19)
#define OV5647_AEC_RSVD_3A08		OV5647_REG8(0x3a08)
#define OV5647_AEC_RSVD_3A09		OV5647_REG8(0x3a09)
#define OV5647_AEC_RSVD_3A0A		OV5647_REG8(0x3a0a)
#define OV5647_AEC_RSVD_3A0B		OV5647_REG8(0x3a0b)
#define OV5647_AEC_RSVD_3A0D		OV5647_REG8(0x3a0d)
#define OV5647_AEC_RSVD_3A0E		OV5647_REG8(0x3a0e)
#define OV5647_AEC_RSVD_3A0F		OV5647_REG8(0x3a0f)
#define OV5647_AEC_RSVD_3A10		OV5647_REG8(0x3a10)
#define OV5647_AEC_RSVD_3A1B		OV5647_REG8(0x3a1b)
#define OV5647_AEC_RSVD_3A1E		OV5647_REG8(0x3a1e)
#define OV5647_AEC_RSVD_3A11		OV5647_REG8(0x3a11)
#define OV5647_AEC_RSVD_3A1F		OV5647_REG8(0x3a1f)

static const struct video_reg ov5647_init_regs[] = {
	/* PLL + MIPI-TX pad-drive init -- see the block comment above OV5647_SC_PLL_CTRL1 for why
	 * this must run here (software standby, before ov5647_lane_park()) and not be reordered.
	 */
	{OV5647_SC_PLL_CTRL3, OV5647_PLL_PREDIV}, /* bits[3:0] prediv=3, bit4 root_div=0 (/1) */
	{OV5647_SC_PLL_MULTIPLIER, OV5647_PLL_MULT}, /* full-byte multiplier = 105 */
	{OV5647_SC_PLL_CTRL1, (OV5647_PLL_SYS_DIV << 4) | 0x1}, /* bits[7:4] sysdiv=2; low nibble
								  * unchanged from power-on (0x11)
								  */
	{OV5647_SC_PLL_CTRL_RSVD_303C, 0x11},
	{OV5647_IO_PAD_CTRL0, 0xf0}, /* pgm_lptx=3 (max, bits[5:4]), pgm_vcm=3 (bits[7:6]); see the
				      * file header's bisection for why pgm_lptx must be 3, not
				      * mainline's 2
				      */
	{OV5647_IO_PAD_CTRL1, 0xf8},
	{OV5647_IO_PAD_CTRL2, 0xf0},
	{OV5647_SC_CLKRST_RSVD_3106, 0xf5},
	{OV5647_SC_MIPI_PHY, OV5647_MIPI_PAD_ENABLE},
	/* Drive the frame length from TIMING_VTS instead of letting the AEC stretch it */
	{OV5647_MANUAL_CTRL, OV5647_MANUAL_CTRL_VTS},
	{OV5647_VTS_DIFF, 0},
	{OV5647_TIMING_HTS_REG, OV5647_HTS},
	/* Common analog bias / misc (AUTHORIZED LOCAL DIVERGENCE #3, bench run 58 phase PC) */
	{OV5647_ANALOG_RSVD_370C, 0x03},
	{OV5647_ANALOG_RSVD_3630, 0x2e},
	{OV5647_ANALOG_RSVD_3632, 0xe2},
	{OV5647_ANALOG_RSVD_3633, 0x23},
	{OV5647_ANALOG_RSVD_3634, 0x44},
	{OV5647_ANALOG_RSVD_3620, 0x64},
	{OV5647_ANALOG_RSVD_3621, 0xe0},
	{OV5647_ANALOG_RSVD_3600, 0x37},
	{OV5647_ANALOG_RSVD_3704, 0xa0},
	{OV5647_ANALOG_RSVD_3703, 0x5a},
	{OV5647_ANALOG_RSVD_3715, 0x78},
	{OV5647_ANALOG_RSVD_3717, 0x01},
	{OV5647_ANALOG_RSVD_3731, 0x02},
	{OV5647_ANALOG_RSVD_370B, 0x60},
	{OV5647_ANALOG_RSVD_3705, 0x1a},
	{OV5647_ANALOG_RSVD_3F05, 0x02},
	{OV5647_ANALOG_RSVD_3F06, 0x10},
	{OV5647_ANALOG_RSVD_3F01, 0x0a},
	{OV5647_ANALOG_RSVD_3C01, 0x80},
	{OV5647_ANALOG_RSVD_3B07, 0x0c},
	{OV5647_ANALOG_RSVD_3636, 0x06},
	{OV5647_ANALOG_RSVD_3827, 0xec},
	/* BLC (bench run 58 phase PD) */
	{OV5647_BLC_RSVD_4001, 0x02},
	{OV5647_BLC_RSVD_4004, 0x02},
	{OV5647_BLC_RSVD_4000, 0x09},
	{OV5647_BLC_RSVD_4050, 0x6e},
	{OV5647_BLC_RSVD_4051, 0x8f},
	/* AEC target/limits (bench run 58 phase PF); left unchanged -- see the file header's
	 * low-light note, a scene limitation, not a driver bug.
	 */
	{OV5647_AEC_RSVD_3A18, 0x00},
	{OV5647_AEC_RSVD_3A19, 0xf8},
	{OV5647_AEC_RSVD_3A08, 0x01},
	{OV5647_AEC_RSVD_3A09, 0x2e},
	{OV5647_AEC_RSVD_3A0A, 0x00},
	{OV5647_AEC_RSVD_3A0B, 0xfb},
	{OV5647_AEC_RSVD_3A0D, 0x02},
	{OV5647_AEC_RSVD_3A0E, 0x01},
	{OV5647_AEC_RSVD_3A0F, 0x58},
	{OV5647_AEC_RSVD_3A10, 0x50},
	{OV5647_AEC_RSVD_3A1B, 0x58},
	{OV5647_AEC_RSVD_3A1E, 0x50},
	{OV5647_AEC_RSVD_3A11, 0x60},
	{OV5647_AEC_RSVD_3A1F, 0x28},
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
static const uint32_t ov5647_framerates[] = {10, 15, 30, 45, 60, 90, 120};

static uint32_t ov5647_frmrate_to_vts(const struct device *dev, uint32_t frmrate)
{
	const struct ov5647_config *cfg = dev->config;

	return cfg->pixel_rate / (OV5647_HTS * frmrate);
}

static int ov5647_set_window(const struct device *dev, uint32_t width, uint32_t height)
{
	const struct ov5647_config *cfg = dev->config;
	uint32_t x_start = OV5647_X_ADDR_START + (OV5647_FULL_WIDTH - width) / 2;
	uint32_t y_start = OV5647_Y_ADDR_START + (OV5647_FULL_HEIGHT - height) / 2;
	const struct video_reg regs[] = {
		{OV5647_TIMING_X_ADDR_START, x_start},
		{OV5647_TIMING_Y_ADDR_START, y_start},
		{OV5647_TIMING_X_ADDR_END, x_start + width + OV5647_WINDOW_MARGIN - 1},
		{OV5647_TIMING_Y_ADDR_END, y_start + height + OV5647_WINDOW_MARGIN - 1},
		{OV5647_TIMING_X_OUTPUT_SIZE, width},
		{OV5647_TIMING_Y_OUTPUT_SIZE, height},
	};

	return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
}

/*
 * Pick the window/output-size/subsample/binning/analog register set for @p width x @p height
 * (AUTHORIZED LOCAL DIVERGENCE #3, issue #2248) and write it as ONE coherent block -- see the
 * ORDERING TRAP note in the file header for why binning must never be written apart from the
 * full-array window it depends on. 640x480 gets the bench-proven full-FOV binned mode (run 60);
 * every other size keeps today's centred-crop window (ov5647_set_window()) but now also
 * explicitly re-asserts the 1:1 subsample/binning/analog values, so a prior 640x480 selection
 * can never leave binning armed on the new crop window.
 */
static int ov5647_set_mode_regs(const struct device *dev, uint32_t width, uint32_t height)
{
	const struct ov5647_config *cfg = dev->config;
	int ret;

	if (width == OV5647_MODE_640X480_WIDTH && height == OV5647_MODE_640X480_HEIGHT) {
		const struct video_reg regs[] = {
			{OV5647_TIMING_X_ADDR_START, OV5647_FULLFOV_X_ADDR_START},
			{OV5647_TIMING_Y_ADDR_START, OV5647_FULLFOV_Y_ADDR_START},
			{OV5647_TIMING_X_ADDR_END, OV5647_FULLFOV_X_ADDR_END},
			{OV5647_TIMING_Y_ADDR_END, OV5647_FULLFOV_Y_ADDR_END},
			{OV5647_TIMING_X_OUTPUT_SIZE, width},
			{OV5647_TIMING_Y_OUTPUT_SIZE, height},
			{OV5647_TIMING_X_INC, OV5647_SUBSAMPLE_BINNED},
			{OV5647_TIMING_Y_INC, OV5647_SUBSAMPLE_BINNED},
			{OV5647_TIMING_TC_REG21, OV5647_TC_REG21_BINNED},
			{OV5647_TIMING_TC_REG20, OV5647_TC_REG20_BINNED},
			{OV5647_ANALOG_CTRL12, OV5647_ANALOG_CTRL12_BINNED},
			{OV5647_ANALOG_CTRL18, OV5647_ANALOG_CTRL18_BINNED},
			{OV5647_SENSOR_CTRL08, OV5647_SENSOR_CTRL08_BINNED},
			{OV5647_SENSOR_CTRL09, OV5647_SENSOR_CTRL09_BINNED},
		};

		return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
	}

	ret = ov5647_set_window(dev, width, height);
	if (ret < 0) {
		return ret;
	}

	const struct video_reg regs[] = {
		{OV5647_TIMING_X_INC, OV5647_SUBSAMPLE_1TO1},
		{OV5647_TIMING_Y_INC, OV5647_SUBSAMPLE_1TO1},
		{OV5647_TIMING_TC_REG20, OV5647_TC_REG20_1TO1},
		{OV5647_TIMING_TC_REG21, OV5647_TC_REG21_1TO1},
		{OV5647_ANALOG_CTRL12, OV5647_ANALOG_CTRL12_1TO1},
		{OV5647_ANALOG_CTRL18, OV5647_ANALOG_CTRL18_1TO1},
		{OV5647_SENSOR_CTRL08, OV5647_SENSOR_CTRL08_1TO1},
		{OV5647_SENSOR_CTRL09, OV5647_SENSOR_CTRL09_1TO1},
	};

	return video_write_cci_multiregs(&cfg->i2c, regs, ARRAY_SIZE(regs));
}

static int ov5647_enum_frmival(const struct device *dev, struct video_frmival_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(ov5647_framerates)) {
		return -EINVAL;
	}

	/* A frame rate is only reachable if its frame length still covers the read-out */
	if (ov5647_frmrate_to_vts(dev, ov5647_framerates[fie->index]) <
	    fie->format->height + OV5647_VBLANK_MIN) {
		return -EINVAL;
	}

	fie->type = VIDEO_FRMIVAL_TYPE_DISCRETE;
	fie->discrete.numerator = 1;
	fie->discrete.denominator = ov5647_framerates[fie->index];

	return 0;
}

static int ov5647_set_frmival(const struct device *dev, struct video_frmival *frmival)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct video_frmival_enum fie = {
		.discrete = *frmival,
		.type = VIDEO_FRMIVAL_TYPE_DISCRETE,
		.format = &data->fmt,
	};
	int ret;

	ret = video_closest_frmival(dev, &fie);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_TIMING_VTS_REG,
				  ov5647_frmrate_to_vts(dev, ov5647_framerates[fie.index]));
	if (ret < 0) {
		return ret;
	}

	*frmival = fie.discrete;
	data->frmrate = ov5647_framerates[fie.index];

	return 0;
}

static int ov5647_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct ov5647_data *data = dev->data;

	frmival->numerator = 1;
	frmival->denominator = data->frmrate;

	return 0;
}

/* Forward declaration: ov5647_set_fmt() re-parks after its writes (see below), but the park
 * helper is defined further down, alongside the registers it uses.
 */
static int ov5647_lane_park(const struct device *dev);

static int ov5647_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct video_frmival frmival = {.numerator = 1, .denominator = data->frmrate};
	size_t idx;
	int ret;

	if (data->streaming) {
		LOG_ERR("Cannot change the format while streaming");
		return -EBUSY;
	}

	ret = video_format_caps_index(ov5647_fmts, fmt, &idx);
	if (ret < 0) {
		LOG_ERR("Format '%s' %ux%u not supported", VIDEO_FOURCC_TO_STR(fmt->pixelformat),
			fmt->width, fmt->height);
		return -ENOTSUP;
	}

	/* Centering the window on the pixel array must not shift the Bayer order */
	if (fmt->width % ov5647_fmts[idx].width_step != 0 ||
	    fmt->height % ov5647_fmts[idx].height_step != 0) {
		LOG_ERR("Resolution %ux%u is not a multiple of %ux%u", fmt->width, fmt->height,
			ov5647_fmts[idx].width_step, ov5647_fmts[idx].height_step);
		return -EINVAL;
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
		return ret;
	}

	ret = video_modify_cci_reg(&cfg->i2c, OV5647_SC_PLL_CTRL0, OV5647_MIPI_BIT_MODE,
				   idx == OV5647_FMT_SBGGR8 ? OV5647_MIPI_BIT_MODE_RAW8
							    : OV5647_MIPI_BIT_MODE_RAW10);
	if (ret < 0) {
		return ret;
	}

	ret = ov5647_set_mode_regs(dev, fmt->width, fmt->height);
	if (ret < 0) {
		return ret;
	}

	data->fmt = *fmt;

	/* The reachable frame rates depend on the height that was just programmed */
	ret = ov5647_set_frmival(dev, &frmival);
	if (ret < 0) {
		return ret;
	}

	return ov5647_lane_park(dev);
}

static int ov5647_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct ov5647_data *data = dev->data;

	*fmt = data->fmt;

	return 0;
}

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
	int ret;

	ret = video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, OV5647_MODE_SELECT_STREAMING);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV5647_MIPI_CTRL00,
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

static int ov5647_set_stream(const struct device *dev, bool on, enum video_buf_type type)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	int ret;

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

	ret = video_write_cci_reg(&cfg->i2c, OV5647_MODE_SELECT, OV5647_MODE_SELECT_STREAMING);
	if (ret < 0) {
		return ret;
	}

	data->streaming = true;

	return 0;
}

static int ov5647_set_ctrl_gain(const struct device *dev)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct ov5647_ctrls *ctrls = &data->ctrls;
	int ret;

	ret = video_modify_cci_reg(&cfg->i2c, OV5647_MANUAL_CTRL, OV5647_MANUAL_CTRL_AGC,
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
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct ov5647_ctrls *ctrls = &data->ctrls;
	int ret;

	ret = video_modify_cci_reg(&cfg->i2c, OV5647_MANUAL_CTRL, OV5647_MANUAL_CTRL_AEC,
				   ctrls->exposure_auto.val == VIDEO_EXPOSURE_MANUAL
					   ? OV5647_MANUAL_CTRL_AEC
					   : 0);
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
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	int32_t val = data->ctrls.test_pattern.val;

	return video_write_cci_reg(&cfg->i2c, OV5647_ISP_CTRL3D,
				   val == 0 ? 0 : (OV5647_TEST_PATTERN_ENABLE | (val - 1)));
}

static int ov5647_set_ctrl(const struct device *dev, uint32_t cid)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct ov5647_ctrls *ctrls = &data->ctrls;

	switch (cid) {
	case VIDEO_CID_AUTOGAIN:
		return ov5647_set_ctrl_gain(dev);
	case VIDEO_CID_EXPOSURE_AUTO:
	case VIDEO_CID_EXPOSURE:
		return ov5647_set_ctrl_exposure(dev);
	case VIDEO_CID_HFLIP:
		return video_modify_cci_reg(&cfg->i2c, OV5647_TIMING_TC_REG21,
					    OV5647_TC_REG21_MIRROR,
					    ctrls->hflip.val != 0 ? OV5647_TC_REG21_MIRROR : 0);
	case VIDEO_CID_VFLIP:
		return video_modify_cci_reg(&cfg->i2c, OV5647_TIMING_TC_REG20,
					    OV5647_TC_REG20_VFLIP,
					    ctrls->vflip.val != 0 ? OV5647_TC_REG20_VFLIP : 0);
	case VIDEO_CID_TEST_PATTERN:
		return ov5647_set_ctrl_test_pattern(dev);
	default:
		return -ENOTSUP;
	}
}

static int ov5647_get_volatile_ctrl(const struct device *dev, uint32_t cid)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	uint32_t gain;
	int ret;

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
	.set_format = ov5647_set_fmt,
	.get_format = ov5647_get_fmt,
	.get_caps = ov5647_get_caps,
	.set_stream = ov5647_set_stream,
	.set_ctrl = ov5647_set_ctrl,
	.get_volatile_ctrl = ov5647_get_volatile_ctrl,
	.set_frmival = ov5647_set_frmival,
	.get_frmival = ov5647_get_frmival,
	.enum_frmival = ov5647_enum_frmival,
};

static const char *const ov5647_exposure_auto_menu[] = {
	"Auto Mode",
	"Manual Mode",
	NULL,
};

static const char *const ov5647_test_pattern_menu[] = {
	"Off",
	"Color bar",
	"Color square",
	"Random data",
	NULL,
};

static int ov5647_init_ctrls(const struct device *dev)
{
	const struct ov5647_config *cfg = dev->config;
	struct ov5647_data *data = dev->data;
	struct ov5647_ctrls *ctrls = &data->ctrls;
	int ret;

	ret = video_init_ctrl(&ctrls->auto_gain, dev, VIDEO_CID_AUTOGAIN,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 1});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->gain, dev, VIDEO_CID_ANALOGUE_GAIN,
			      (struct video_ctrl_range){.min = 0, .max = OV5647_AGC_GAIN_MAX,
							.step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}

	ret = video_auto_cluster_ctrl(&ctrls->auto_gain, 2, true);
	if (ret < 0) {
		return ret;
	}

	ret = video_init_menu_ctrl(&ctrls->exposure_auto, dev, VIDEO_CID_EXPOSURE_AUTO,
				   VIDEO_EXPOSURE_AUTO, ov5647_exposure_auto_menu);
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->exposure, dev, VIDEO_CID_EXPOSURE,
			      (struct video_ctrl_range){.min = 0, .max = OV5647_EXPOSURE_MAX,
							.step = 1,
							.def = OV5647_EXPOSURE_DEFAULT});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->hflip, dev, VIDEO_CID_HFLIP,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->vflip, dev, VIDEO_CID_VFLIP,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_menu_ctrl(&ctrls->test_pattern, dev, VIDEO_CID_TEST_PATTERN, 0,
				   ov5647_test_pattern_menu);
	if (ret < 0) {
		return ret;
	}

	return video_init_ctrl(&ctrls->pixel_rate, dev, VIDEO_CID_PIXEL_RATE,
			       (struct video_ctrl_range){.min64 = cfg->pixel_rate,
							 .max64 = cfg->pixel_rate,
							 .step64 = 1,
							 .def64 = cfg->pixel_rate});
}

static int ov5647_init(const struct device *dev)
{
	const struct ov5647_config *cfg = dev->config;
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width = OV5647_FULL_WIDTH,
		.height = OV5647_FULL_HEIGHT,
	};
	uint32_t chip_id;
	int ret;

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

	ret = video_write_cci_multiregs(&cfg->i2c, ov5647_init_regs,
					ARRAY_SIZE(ov5647_init_regs));
	if (ret < 0) {
		return ret;
	}

	/* Route the pixel stream to the MIPI transmitter, keeping the power-on lane count */
	ret = video_modify_cci_reg(&cfg->i2c, OV5647_SC_MIPI_SC_CTRL,
				   OV5647_PHY_PD_MIPI | OV5647_PHY_PD_LPRX | OV5647_MIPI_EN,
				   OV5647_MIPI_EN);
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
#define OV5647_GET_PWDN_GPIO(n) .pwdn_gpio = GPIO_DT_SPEC_INST_GET_OR(n, pwdn_gpios, {0}),
#else
#define OV5647_GET_PWDN_GPIO(n)
#endif

#define OV5647_EP(n) DT_CHILD(DT_INST_CHILD(n, port), endpoint)
#define OV5647_INPUT_CLK(n) DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency)

#define OV5647_INIT(n)                                                                             \
	BUILD_ASSERT(DT_PROP_OR(OV5647_EP(n), bus_type, VIDEO_BUS_TYPE_CSI2_DPHY) ==               \
			     VIDEO_BUS_TYPE_CSI2_DPHY,                                             \
		     "Only the MIPI CSI-2 D-PHY interface is supported");                          \
	BUILD_ASSERT(DT_PROP_LEN_OR(OV5647_EP(n), data_lanes, 2) == 2,                             \
		     "Only the two data lanes mode is supported");                                 \
	BUILD_ASSERT(IN_RANGE(OV5647_INPUT_CLK(n), OV5647_INPUT_CLK_MIN, OV5647_INPUT_CLK_MAX),    \
		     "XVCLK must be between 6 MHz and 27 MHz");                                    \
                                                                                                   \
	static struct ov5647_data ov5647_data_##n = {                                              \
		.frmrate = 15,                                                                     \
	};                                                                                         \
                                                                                                   \
	static const struct ov5647_config ov5647_cfg_##n = {                                       \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
		OV5647_GET_PWDN_GPIO(n)                                                            \
		.pixel_rate = OV5647_PIXEL_RATE(OV5647_INPUT_CLK(n)),                              \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &ov5647_init, NULL, &ov5647_data_##n, &ov5647_cfg_##n,            \
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &ov5647_driver_api);        \
                                                                                                   \
	VIDEO_DEVICE_DEFINE(ov5647_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(OV5647_INIT)
