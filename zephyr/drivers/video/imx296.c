/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR-0017-ADJACENT, PARTIALLY BENCH-VERIFIED ======
 * Sony IMX296 1.58 MP global-shutter MIPI CSI-2 image sensor, authored from
 * the Sony IMX296 datasheet (register map + timing sections). No
 * permissive-licence Zephyr/vendor driver exists to consume for this part:
 * upstream Zephyr v4.4.1 ships no "sony,imx296" driver, hal_alif carries no
 * IMX296 register lib, and Espressif esp_cam_sensor has no IMX296 port (it
 * has ov9281, consumed separately in ov9281.c). The only other IMX296
 * drivers known to exist are Linux's (GPL-2.0) and libcamera's helpers
 * (also GPL/LGPL) -- neither is consumable under this project's licence.
 * Linux's imx296.c was used strictly as a REGISTER-NAME reference for a
 * handful of names this datasheet leaves unnamed (e.g. SENSOR_INFO,
 * 0x3148) -- it was not opened for, and no code, text or table from it was
 * copied into, this file; every register address and value below is
 * independently cited to a datasheet section/table in the comment above it,
 * with ONE bench-derived exception: IMX296_REG_CSI_LANE_HS (0x3005 = 0xF0,
 * see that macro's own comment) has no datasheet citation at all -- it is a
 * silicon fact found by sweeping an undocumented register address, not a
 * documented one this driver merely transcribes.
 *
 * BENCH STATUS (issue #2287, E1M-AEN803 2026W36-0001, csi_i2c = I2C1 @
 * 0x49011000): the I2C identity path is silicon-verified (bench run 229 --
 * SENSOR_INFO reads 0x4A00, the colour-IMX296LQR signature this driver
 * expects). On a later bench pass, on the same silicon, the sensor also
 * streams -- the CSI-2 host's frame counter advances once XMSTA starts
 * master-mode free-run. A further bench pass isolated the data lane
 * specifically: WITHOUT IMX296_REG_CSI_LANE_HS (0x3005 = 0xF0, see that
 * macro's comment above) the data lane never leaves LP-11 at all -- no HS
 * output whatsoever; WITH it written (bench runs #263/#264), HS data does
 * arrive at the CSI-2 host. Whether 0xF0 selects some diagnostic/test-pattern
 * mode versus normal operation is NOT established from the datasheet (this
 * register is undocumented) or from image content (see below) -- it is
 * simply the value that was swept and found to unblock HS output.
 *
 * With the DW CSI-2 host's IPI switched to Controller timing mode (fixed
 * HLINE/VTOTAL matched to this sensor's HMAX/VMAX, see
 * raspberry_pi_global_shutter_camera.overlay's &csi node and its
 * derivation comment) the earlier "Camera" timing mode's complete failure
 * to emit any IPI line is RESOLVED: Controller mode captures full
 * 1456x1088 frames with zero IPI-FIFO-overflow events across a full
 * capture run (csi-hsd 503, see the overlay). Current state (Stage A, bench
 * run 292): a single free-run capture reads a clean, faint but real image
 * (mean pixel value 61.19, no RAW10 byte-phase-slip pattern). Run 292's
 * build had CONFIG_LOG unset, so its clean console says nothing about
 * INT_IPI_PIXEL_IF_HLINE_ERR/_FIFO_OVERFLOW -- their status on the product
 * build is UNKNOWN, not "not seen". Continuous streaming, ISP-Pico, AE and
 * fast-trigger mode are not yet bench-verified. See changelog.d/2287.md
 * (issue #2287) for the full bring-up history and every bench number.
 *
 * LANE-PARK / D-PHY BEHAVIOUR: unlike OV5647 (issue #2248,
 * ov5647_lane_park()), this datasheet documents no register that forces the
 * CSI-2 CLOCK lane to Stop-state (LP-11) short of exiting STANDBY into full
 * master-mode streaming -- imx296_init() leaves the sensor in STANDBY (no
 * MIPI output at all), so the CSI-2 host's D-PHY Stop-state wait
 * (dphy_dw_slave_setup(), called from csi2_dw_configure() at set_format
 * time) would otherwise time out fatally before streaming ever starts, the
 * same shape OV5647 hit before ov5647_lane_park() fixed it there. This is
 * an INFERENCE from the datasheet and this driver's own init() ordering,
 * not a bench measurement of the D-PHY's Stop-state register during
 * STANDBY. Because no sensor-side fix is available, the shield instead sets
 * `no-lp11-clock-lane-park` on this sensor's own CSI-2 endpoint (see
 * raspberry_pi_global_shutter_camera.overlay and the property's doc comment
 * in zephyr/dts/bindings/video/sony,imx296.yaml) so the D-PHY wait skips
 * only the clock-lane Stop-state bit for this sensor; the data-lane
 * Stop-state check and the wait's timeout stay fatal for this sensor and
 * every other, including OV5647 -- that fatality is what caught OV5647's
 * own silicon defect (#2248) and must not be weakened globally.
 *
 * RETIREMENT: upstream this driver to Zephyr (drivers/video/) the moment a
 * native "sony,imx296" driver lands there, and delete this file + its
 * Kconfig + binding. See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 */

#define DT_DRV_COMPAT sony_imx296

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/dt-bindings/video/video-interfaces.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "video_common.h"
#include "video_ctrls.h"
#include "video_device.h"

LOG_MODULE_REGISTER(imx296, CONFIG_VIDEO_LOG_LEVEL);

/*
 * Driver-private control, V4L2/Zephyr vendor-CID convention (VIDEO_CID_PRIVATE_BASE, see
 * video-controls.h) -- precedent: zephyr/drivers/video/video_emul_imager.c's
 * EMUL_IMAGER_CID_CUSTOM. No standard Zephyr video CID exists for "external trigger vs free-run
 * exposure", so this rides the vendor-private range rather than inventing a cross-sensor standard
 * one driver's worth of evidence cannot justify. Selects between this sensor's two Global Shutter
 * sub-modes -- see IMX296_REG_TRIGEN's comment below for why only fast trigger mode is reachable
 * on this hardware. 0 (default) = free-run (current, unchanged behaviour); 1 = fast trigger mode
 * (issue #2287): exposure is then controlled by the width of an external low pulse on the XTRIG
 * pin rather than by VIDEO_CID_EXPOSURE/SHS. The mode only takes effect on the next
 * video_stream_start() -- "Mode Transitions of Global Shutter Operation" (page 66) requires the
 * TRIGEN/LOWLAGTRG switch to happen "via sensor standby", so imx296_set_ctrl() rejects a write to
 * this control with -EBUSY while already streaming, rather than silently queuing it for whatever
 * stop/start cycle comes next.
 */
#define IMX296_CID_TRIGGER_MODE      (VIDEO_CID_PRIVATE_BASE + 0x01)
#define IMX296_TRIGGER_MODE_FREE_RUN 0
#define IMX296_TRIGGER_MODE_EXTERNAL 1

/*
 * Datasheet "Setting Registers Using Serial Communication" -> "Description
 * of Setting Registers (I2C)": the sensor answers on one of three 7-bit CCI
 * addresses depending on the SLAMODE pin strap (0x36 / 0x37), or 0x1A
 * regardless of SLAMODE polarity ("SLAVE Address (SLAMODE = 0 / 1)"). 0x1A
 * is used here since the module's SLAMODE strap is not under this driver's
 * control and 0x1A works either way -- see zephyr/dts/bindings/video/
 * sony,imx296.yaml. Bench run 229 confirmed the module answers at 0x1A.
 */

#define IMX296_REG8(addr)  ((addr) | VIDEO_REG_ADDR16_DATA8)
#define IMX296_REG16(addr) ((addr) | VIDEO_REG_ADDR16_DATA16_LE)
#define IMX296_REG24(addr) ((addr) | VIDEO_REG_ADDR16_DATA24_LE)

/* Register Map, Chip ID = 02h (page 34): standby control, bit0, reflect "I" (immediate) */
#define IMX296_REG_STANDBY     IMX296_REG8(0x3000)
#define IMX296_STANDBY_STANDBY BIT(0)

/*
 * Register Map, Chip ID = 02h (page 34) + "Slave Mode and Master Mode"
 * (page 55): XMSTA bit0, 0 = master-mode operation start, 1 = stop (POR
 * default). The 15-pin RPi-style CSI connector this part sits behind
 * carries no XVS/XHS lines, so slave mode (externally clocked) is not
 * reachable -- this driver always runs the sensor in master mode, deriving
 * its own frame timing from VMAX/HMAX and INCK per "Slave Mode and Master
 * Mode".
 */
#define IMX296_REG_XMSTA  IMX296_REG8(0x300A)
#define IMX296_XMSTA_STOP BIT(0)

/*
 * Register Map, Chip ID = 03h (page 40, I2C bank 0x31xx): SENSOR_INFO, a
 * 16-bit little-endian value at CCI address 0x3148 -- UNDOCUMENTED. Page 40
 * lists Chip ID = 03h's entire register bank as "Please refer to the other
 * register map file for the register that has not been described"; no
 * register name, bit map or description for 0x3148/0x3149 appears anywhere
 * in this public preliminary datasheet. This driver treats it purely as an
 * opaque post-standby-exit IDENTITY SIGNATURE, not a documented chip-ID
 * register in the conventional (named bit-field) sense.
 *
 * Silicon-confirmed, bench run 229 (issue #2287, E1M-AEN803 2026W36-0001):
 * a real INNO-MAKER CAM-IMX296RAW-TRIGGER (Sony IMX296LQR-C, colour) module
 * read SENSOR_INFO = 0x4A00 after STANDBY was cancelled (see the comment on
 * IMX296_SENSOR_INFO_LQR_COLOUR below for why "after cancelling STANDBY" --
 * the value was not sampled with the sensor left in standby, so that
 * precondition is unconfirmed, not just unneeded). 0x4A00 is the ONLY
 * silicon sample this driver has ever seen: which bit(s) of the 16-bit
 * value encode "model" vs "mono/colour" is NOT independently established --
 * a second bench pass against a mono IMX296LLR module would be needed to
 * isolate that. This driver therefore checks the WHOLE 16-bit value against
 * the one bench-confirmed colour signature and rejects everything else
 * (including a hypothetical mono module) as -ENODEV, rather than decoding
 * bits it cannot back with evidence.
 */
#define IMX296_REG_SENSOR_INFO IMX296_REG16(0x3148)

/*
 * Bench-observed SENSOR_INFO value for the colour IMX296LQR-C variant (the
 * only variant this driver's fixed SRGGB10P format targets) -- see the
 * IMX296_REG_SENSOR_INFO comment above. NOT derived from the datasheet;
 * this is a silicon fact, not a documented constant.
 */
#define IMX296_SENSOR_INFO_LQR_COLOUR 0x4A00u

/*
 * Register Map, Chip ID = 13h (page 43): a byte at CCI address 0x418C
 * (4-wire address 0x8C) with no register name given in the datasheet's own
 * register map; only its per-INCK setting values are documented, in both
 * the Chip ID = 13h register-map entry (page 43) and "Register List of
 * All-pixel scan mode" (page 49). Left as an opaque INCK-dependent byte
 * rather than inventing a name for it.
 */
#define IMX296_REG_CSI_TIMING IMX296_REG8(0x418C)

/*
 * BENCH-DERIVED, NOT DATASHEET-DOCUMENTED (issue #2287, E1M-AEN803
 * 2026W36-0001, colour IMX296LQR-C module): Chip ID = 02h's own register map
 * (page 34) documents 0x3000 (STANDBY) then jumps straight to 0x3008
 * (REGHOLD) -- 0x3001-0x3007, including this byte, fall in the "please
 * refer to the other register map file for the register that has not been
 * described" gap, the same category as SENSOR_INFO (0x3148) above. The
 * datasheet says nothing about this address's bits, name or reset value.
 * On the bench: with this byte left at its POR value the CSI-2 data lane
 * never leaves LP-11 (no HS output on the data lane at all, confirmed by
 * reading the D-PHY RX lane-status / Stop-state registers over SWD --
 * CSI_PHY_RX at 0x49033048 and CSI_PHY_STOPSTATE at 0x4903304C); writing
 * 0xF0 here (bench runs #263/#264 in the issue's bench log) is what makes
 * HS data start arriving. Whether 0xF0 selects some diagnostic/test-pattern
 * mode, or is simply the normal operating value for this undocumented
 * register, is NOT established -- it is the value that was swept and found
 * to unblock HS output, nothing more. Written unconditionally in
 * imx296_init() below on that basis -- a silicon fact, not a documented
 * register write.
 */
#define IMX296_REG_CSI_LANE_HS IMX296_REG8(0x3005)
#define IMX296_CSI_LANE_HS_VAL 0xF0u

/*
 * Register Map, Chip ID = 04h (page 41) + "Register List of All-pixel scan
 * mode" (page 49): BLKLEVEL[11:0] at 0x3254 (LSB), reflect "V". Black-level
 * offset; the datasheet marks its own POR default (0x03C) "Recommended
 * value" and lists it explicitly in the All-pixel-scan-mode register table
 * alongside VMAX/HMAX/INCKSEL/CSI_TIMING. Written here (even though it
 * equals the POR default) for the same warm-boot-state reason SHS/GAIN/
 * REVERSE are written explicitly below: a module that stayed powered across
 * a prior session could carry a different BLKLEVEL than POR.
 */
#define IMX296_REG_BLKLEVEL IMX296_REG16(0x3254)
#define IMX296_BLKLEVEL     0x03Cu

/*
 * Register Map, Chip ID = 02h (page 34): TRIGEN bit0, reflect "S" (standby-set, applied on
 * standby cancel). "Global shutter mode setting", 0: Normal mode 1: Trigger mode. Also listed in
 * "Global Shutter (Sequential Trigger Mode) Operation" -> "Register List of shutter setting"
 * (page 62) and "Global Shutter (Fast Trigger Mode) Operation" -> "Register List of shutter
 * setting" (page 64) -- same register, same address, in both trigger sub-modes.
 *
 * Only FAST trigger mode is wired by this driver (see IMX296_REG_LOWLAGTRG below): "Global
 * Shutter (Sequential Trigger Mode) Operation" (page 62) states "This function is slave mode
 * only", and this driver -- like every sensor on the RPi-style 15-pin CSI connector it sits
 * behind -- has no XVS/XHS lines wired (see IMX296_REG_XMSTA's comment above) and so can never
 * reach slave mode. Fast trigger mode's own section (page 64) states "This mode supports Master
 * mode only", which is the only mode this hardware can ever be in.
 */
#define IMX296_REG_TRIGEN     IMX296_REG8(0x300B)
#define IMX296_TRIGEN_TRIGGER BIT(0)

/*
 * Register Map, Chip ID = 02h (page 40): LOWLAGTRG bit0, reflect "S". "Selection of trigger
 * mode", 0: Except for Fast trigger mode, 1: Fast trigger mode. Set alongside IMX296_REG_TRIGEN
 * (=1) to select FAST trigger mode specifically, per "Global Shutter (Fast Trigger Mode)
 * Operation" -> "Register List of shutter setting" (page 64).
 */
#define IMX296_REG_LOWLAGTRG  IMX296_REG8(0x30AE)
#define IMX296_LOWLAGTRG_FAST BIT(0)

/*
 * Register Map, Chip ID = 02h (page 37): SYNCSEL, bits[5:4], reflect "S". "XHS, XVS pin setting",
 * 0h: Normal Output, 3h: Hi-Z; bits[7:6] are datasheet-fixed to 1, all other bits fixed to 0 --
 * POR default for the whole byte is 0xC0 (= bits[7:6]=1, bits[5:4]=0h Normal Output), which is
 * also the value "Global Shutter (Fast Trigger Mode) Operation" -> "Register List of shutter
 * setting" (page 64) lists for entering fast trigger mode. Written explicitly (rather than relied
 * on as POR) for the same warm-boot-state reason BLKLEVEL/SHS/GAIN/REVERSE are below -- a module
 * left running from a prior session could carry a different value here.
 */
#define IMX296_REG_SYNCSEL    IMX296_REG8(0x3036)
#define IMX296_SYNCSEL_NORMAL 0xC0u

/*
 * Register Map, Chip ID = 05h (page 42) + "Register List of ROI mode" /
 * "Restrictions on ROI mode" (pages 51-53): FID0_ROIH1ON bit0 (reflect "V"),
 * FID0_ROIV1ON bit1 (reflect "I") at CCI address 0x3300, POR default 0 (both
 * disabled) -- this is the datasheet's actual all-pixel-scan-mode-vs-
 * windowed-ROI-mode switch ("ROI mode" is entered by ENABLING both bits and
 * programming FID0_ROIPH1/ROIPV1/ROIWH1/ROIWV1 below; "Please set All-pixel
 * scan mode to the settings other than the following" for everything else).
 * This datasheet does not separately document a 0x300D "window mode"
 * register -- 0x300C/0x300D also fall in the same undocumented gap as
 * IMX296_REG_CSI_LANE_HS above (Chip ID = 02h's map jumps 0x300B -> 0x300E,
 * page 34-35). imx296_init() writes this register's disable value
 * explicitly (rather than assuming POR) so a previous ROI-mode session's
 * cropping window cannot leak into a cold boot's default full-frame output,
 * since the sensor module stays powered across a warm SoC reset (see the
 * STANDBY comment on imx296_init() below); imx296_set_stream() (issue
 * #2287 Stage B) then re-drives it to the value matching the format
 * video_set_format() last selected, every stream start -- see the ROI-mode
 * write block there.
 */
#define IMX296_REG_ROI_ENABLE     IMX296_REG8(0x3300)
#define IMX296_ROI_ENABLE_DISABLE 0x00u
#define IMX296_ROI_ENABLE_ENABLE  (BIT(0) | BIT(1)) /* FID0_ROIH1ON | FID0_ROIV1ON */

/*
 * Register Map, Chip ID = 05h (page 42): FID0_ROIPH1[12:0] at CCI 0x3310
 * (LSB)/0x3311 (upper 5 bits), reflect "V"; FID0_ROIPV1[11:0] at 0x3312/
 * 0x3313, reflect "I"; FID0_ROIWH1[12:0] at 0x3314/0x3315, reflect "V";
 * FID0_ROIWV1[11:0] at 0x3316/0x3317, reflect "I" -- all four are 2-byte
 * fields with the unused high bits fixed to 0 (same "16-bit LE register,
 * fewer bits actually wired" shape as IMX296_REG_HMAX above), so
 * IMX296_REG16() covers them exactly like the driver's other multi-byte
 * registers. "Restrictions on ROI mode" (page 53): all four values must be
 * a multiple of 4; ROIWH1 >= 80, ROIWV1 >= 4; ROIPH1+ROIWH1 <= 1456d;
 * ROIPV1+ROIWV1 <= 1088d. imx296_set_stream()'s ROI-mode write block below
 * programs these unconditionally to the one supported 1280x960 crop
 * whenever that is the format video_set_format() selected; a mismatch
 * against these limits would be a bug in that fixed geometry, not a runtime
 * input to validate.
 */
#define IMX296_REG_ROI_POS_H  IMX296_REG16(0x3310)
#define IMX296_REG_ROI_POS_V  IMX296_REG16(0x3312)
#define IMX296_REG_ROI_SIZE_H IMX296_REG16(0x3314)
#define IMX296_REG_ROI_SIZE_V IMX296_REG16(0x3316)

/*
 * Register Map, Chip ID = 02h (page 35) + "Horizontal / Vertical Normal
 * Operation and Inverted Operation" (page 58): VREVERSE bit0, HREVERSE
 * bit1, reflect "V" (frame-synced).
 */
#define IMX296_REG_REVERSE      IMX296_REG8(0x300E)
#define IMX296_REVERSE_VREVERSE BIT(0)
#define IMX296_REVERSE_HREVERSE BIT(1)

/*
 * Register Map, Chip ID = 02h (page 34-35): VMAX[19:0] at 0x3010 (LSB),
 * reflect "V". Number of lines per frame in master mode -- see "Register
 * List of Shutter setting" (page 60) and "Register List of All-pixel scan
 * mode" (page 49).
 */
#define IMX296_REG_VMAX IMX296_REG24(0x3010)

/*
 * Register Map, Chip ID = 02h (page 35-36): HMAX[15:0] at 0x3014 (LSB),
 * reflect "S" (set during standby, applied on standby cancel). Number of
 * (normalised) clocks per line in master mode -- see "Register List of
 * All-pixel scan mode" (page 49).
 */
#define IMX296_REG_HMAX IMX296_REG16(0x3014)

/*
 * Register Map, Chip ID = 02h (page 38-39): INCKSEL0..3, one byte each,
 * reflect "S". "Set according to INCK frequency and drive mode" -- the
 * per-INCK values are given in "Register List of All-pixel scan mode"
 * (page 49).
 */
#define IMX296_REG_INCKSEL0 IMX296_REG8(0x3089)
#define IMX296_REG_INCKSEL1 IMX296_REG8(0x308A)
#define IMX296_REG_INCKSEL2 IMX296_REG8(0x308B)
#define IMX296_REG_INCKSEL3 IMX296_REG8(0x308C)

/*
 * Register Map, Chip ID = 02h (page 38-39): SHS[19:0] at 0x308D (LSB),
 * reflect "V". Shutter sweep time, in line units, from "Calculation
 * Formula of Exposure Time" (page 60):
 *   exposure_time[s] = (1H period) x (lines_per_frame - SHS) + 14.26us
 * "Register List of Shutter setting" (page 60) gives the valid SHS range as
 * [IMX296_SHS_MIN, lines_per_frame - 1] (memory wait time = 4H).
 */
#define IMX296_REG_SHS     IMX296_REG24(0x308D)
#define IMX296_SHS_MIN     4
#define IMX296_SHS_DEFAULT 14 /* POR default (page 39): 0x0000E */

/*
 * Register Map, Chip ID = 04h (page 40-41) + "Gain Adjustment Function"
 * (page 56): GAIN[8:0] at 0x3204 (LSB), reflect "V". Combined analogue +
 * digital gain, 0.1 dB step, 0 (0 dB) to 480 (48.0 dB) -- "Register List of
 * Gain setting" (page 56) gives the valid range as 000h to 1E0h (0 to 480
 * decimal). This driver exposes the whole combined value as a single
 * control, VIDEO_CID_ANALOGUE_GAIN -- there is no separate digital-gain
 * control or register in this datasheet to split it against.
 */
#define IMX296_REG_GAIN IMX296_REG16(0x3204)
#define IMX296_GAIN_MAX 480

/*
 * "Gain Adjustment Function" (page 41 + page 56): GAINDLY[3:0] at 0x3212, an 8-bit register
 * this driver treats as a single control byte (no sub-bit-field split needed -- only ONE value
 * is ever legal, see below). Per page 41's own register table (verbatim): "08h: Gain reflect
 * at the frame" and "09h: Gain reflect at the next frame (Same timing as SHS reflecting
 * output.)" -- every OTHER value is explicitly listed "Setting prohibited", including this
 * register's OWN power-on-reset default, 00h. #2287 (bench runs 304-306, E1M-AEN803
 * 2026W36-0001): this driver never wrote GAINDLY at all before, leaving it at the prohibited
 * POR value the whole time gain has ever been written on this board -- not independently
 * proven to be a cause of any bench symptom (bench runs 307/308 isolated it against the
 * horizontal-band symptom and found no effect -- see changelog.d/2287.md), but a real
 * datasheet violation regardless. 09h is the value used, NOT 08h (an earlier revision of this
 * comment had the two backwards): the datasheet's own "(Same timing as SHS reflecting
 * output.)" parenthetical says so directly -- SHS (IMX296_REG_SHS's own comment, "Calculation
 * Formula of Exposure Time", page 60) already latches on next-frame timing, so 09h keeps GAIN
 * and SHS landing together, on the same frame, instead of GAIN taking effect one frame earlier
 * than the exposure it was paired with by isp_api_wrapper.c's AE writeback.
 */
#define IMX296_REG_GAINDLY    IMX296_REG8(0x3212)
#define IMX296_GAINDLY_DELAY1 0x09

/*
 * "Drive Timing Chart for Serial Output in All-pixel Scan Mode" (page 50):
 * the TRANSMITTED RAW10 (DT 0x2B) frame is the whole 1456x1088 effective
 * array, not the 1440x1080 "recommended recording pixels" of the "Readout
 * Drive Modes" table (page 44). Each RAW10 line carries 8 + 1440 + 8 = 1456
 * pixels (the colour-processing margin is sent, not cropped), and there are
 * 4 + 1080 + 4 = 1088 RAW10 lines per frame. The embedded-data (DT 0x12),
 * NULL (DT 0x10) and vertical-OB (DT 0x37) lines ride other data types
 * ("Image Data Output Format", page 45) that the CSI-2 host's IPI does not
 * pass to the capture side. 1440x1080 is an image-quality recommendation,
 * so cropping to it is the consumer's (or the capture block's) job; the
 * sensor output is 1456x1088.
 */
#define IMX296_WIDTH  1456
#define IMX296_HEIGHT 1088

/*
 * "ROI mode" / "Register List of ROI mode" / "Restrictions on ROI mode"
 * (pages 51-53), issue #2287 Stage B: a second, centred crop window within
 * the same 1456x1088 full-pixel array above. 1280 and 960 are each a
 * multiple of 4 (page 53's requirement); ROIWH1 = 1280 >= the 80-pixel
 * minimum and ROIWV1 = 960 >= the 4-line minimum. IMX296_ROI_POS_H/V below
 * centre it: (IMX296_WIDTH - IMX296_ROI_WIDTH) / 2 = (1456-1280)/2 = 88,
 * (IMX296_HEIGHT - IMX296_ROI_HEIGHT) / 2 = (1088-960)/2 = 64 -- both
 * already multiples of 4, so no rounding was needed. Frame rate on page 53
 * is "1 / ((lines-per-frame or VMAX) x 1H period)" -- in this driver's
 * always-master-mode operation "lines per frame" IS the VMAX register
 * (IMX296_VMAX, unchanged by ROI mode -- see its own comment below), so
 * this crop keeps the same 60.3 frame/s as the full-frame mode as long as
 * VMAX stays >= ROIWV1 + 30 (page 53's datasheet max-rate setting for this
 * ROIWV1 -- treating it as a MINIMUM VMAX here is this driver's own
 * inference, not a datasheet statement, see below): 1118 >= 960 + 30 = 990,
 * satisfied with margin.
 *
 * Page 53's "ROIWV1 + 30" is the MAX-RATE setting for a given ROIWV1 --
 * the smallest legal VMAX, hence the fewest lines per frame and the
 * fastest frame rate this ROIWV1 can reach. Keeping VMAX at the driver's
 * existing fixed 1118 instead of dropping to that max-rate floor (990)
 * is an INFERENCE from the formula, not a bench measurement: reusing the
 * same VMAX both modes already shared was the smaller, more conservative
 * change. Bench runs 294 (full-frame)/295 (ROI, E1M-AEN803
 * 2026W36-0001) confirm the crop itself is correct at this VMAX -- run
 * 295 captured a clean 1280x960 frame (no mod-4-column RAW10
 * byte-phase-slip) -- but neither run measured frame RATE; the 60.3
 * frame/s figure above is still the datasheet-derived value, not a bench
 * measurement. See changelog.d/2287.md's Stage B section for the full
 * run 294/295 write-up.
 */
#define IMX296_ROI_WIDTH  1280
#define IMX296_ROI_HEIGHT 960
#define IMX296_ROI_POS_H  88
#define IMX296_ROI_POS_V  64

/*
 * "Register List of All-pixel scan mode" (page 49), AD = 10 bit / 60.3
 * frame/s column: VMAX = 0x45E (1118 lines/frame), HMAX = 0x44C (1100
 * normalised clocks/line). Both are the SAME across all three supported
 * INCK frequencies -- only INCKSEL0..3 and the 0x418C byte differ per INCK
 * (the datasheet normalises the internal line/frame clock to a fixed
 * INCK-independent rate via those registers).
 */
#define IMX296_VMAX 1118
#define IMX296_HMAX 1100

/*
 * issue #2287: the shield overlay's DW CSI-2 IPI controller-timing values
 * (csi-hsa/hbp/hsd/vsa/vbp/vfp on &csi,
 * zephyr/boards/shields/raspberry_pi_global_shutter_camera/
 * raspberry_pi_global_shutter_camera.overlay) are derived FROM these two
 * constants (sensor line time = IMX296_HMAX / INCK; IPI VTOTAL must equal
 * IMX296_VMAX) and from the CSI IPI pixel clock. Changing either constant
 * here, or the IPI pixel clock the DW CSI-2 host programs
 * (csi2_dw_validate_data(), zephyr/drivers/video/video_csi_dw.c), invalidates
 * that overlay's derivation comment -- recompute and re-bench it too.
 */

/*
 * "Readout Drive Modes" (page 44): All-pixel drive mode, 1 CSI-2 lane,
 * 10-bit A/D, 60.3 frame/s, 1.188 Gbps on the single lane. Per the V4L2
 * MIPI CSI-2 link-frequency convention (the D-PHY clock lane runs at half
 * the bit rate; matches zephyr/drivers/video/imx219.c's own
 * IMX219_2DL_LINK_FREQ derivation for that sensor):
 *   link_freq = per_lane_bit_rate / 2 = 1.188 Gbps / 2 = 594 MHz
 *   pixel_rate = per_lane_bit_rate * lanes / bits_per_pixel
 *              = 1.188 Gbps * 1 / 10 = 118.8 Mpix/s
 * Cross-checked against the table's own numbers using the *total* pixel
 * count per frame (1760 x 1118, the "Total number of pixels" columns, which
 * include blanking) and 60.3 fps: 1760 * 1118 * 60.3 ~= 118.7 Mpix/s. The
 * active 1456x1088 window is a subset of that, so the rate is unchanged.
 */
#define IMX296_LINK_FREQ_HZ  594000000
#define IMX296_PIXEL_RATE_HZ 118800000

/* "List of Exposure Setting" (page 60): 60.3 frame/s = 603/10 frame/s exactly */
#define IMX296_FRMIVAL_NUMERATOR   10
#define IMX296_FRMIVAL_DENOMINATOR 603

/*
 * "Standby mode" (page 54): after STANDBY is cleared, a normal image is
 * output from the 9th frame after "internal regulator stabilization (1 ms
 * or more)". This driver waits out the mandatory 1 ms regulator settling
 * time here -- both for entering the streaming state (imx296_set_stream(),
 * before IMX296_INIT_PERIOD_MS below) and for the momentary standby-exit
 * the SENSOR_INFO probe in imx296_init() needs, which never starts XMSTA
 * and so has no initialization period to wait out.
 */
#define IMX296_STANDBY_SETTLE_MS 1

/*
 * "Standby mode" (page 54): "a normal image is output from the 9 frames
 * after [STANDBY cancel]" -- i.e. an 8-frame initialization period, during
 * which the sensor is still ramping up and every frame is invalid. One
 * frame period is IMX296_VMAX x 1H, and this driver's fixed 60.3 frame/s
 * mode has 1H = 14.81 us ("List of Exposure Setting" / HMAX-derived line
 * time, page 44 + page 53): 8 x IMX296_VMAX x 14.81us ~= 132 ms. Waited out
 * here in imx296_set_stream(dev, true, ...), AFTER the XMSTA write starts
 * master-mode free-run and BEFORE that function returns, so the capture
 * pipeline above it never sees an initialization-period frame: the CSI-2
 * DW host's csi2_dw_stream_start() (video_csi_dw.c) calls this driver's
 * set_stream synchronously before returning to alif_cam_stream_start()
 * (video_alif.c), which only arms the CPI's own capture snapshot
 * (hw_cam_start_video_capture()) once that whole chain has returned --
 * so this wait's caller-blocking is what excludes the init-period frames
 * from ever reaching a buffer. Rounded up from 8 to 9 frames of margin
 * (9 x 1118 x 14.81us ~= 149 ms, ceiling to whole milliseconds).
 *
 * FREE-RUN MODE ONLY. imx296_set_stream() applies this same wait
 * unconditionally, including when `trigger` (IMX296_CID_TRIGGER_MODE) is
 * set -- trigger mode is still master mode, started by this same XMSTA
 * write ("Mode Transitions of Global Shutter Operation", page 66, +
 * "Slave Mode and Master Mode", page 55); only frame OUTPUT and exposure
 * timing become XTRIG-driven after that. Whether the 8-frame
 * initialization period this wait covers still applies unchanged once
 * XTRIG starts pulsing, or runs on a different clock, is not established
 * for trigger mode in this datasheet and has not been bench-checked.
 * Trigger-mode init timing is UNHANDLED and UNBENCHED; this wait is a
 * known-good value for free-run
 * only.
 */
#define IMX296_INIT_PERIOD_MS 150

struct imx296_inck_regs {
	uint32_t hz;
	uint8_t  incksel0;
	uint8_t  incksel1;
	uint8_t  incksel2;
	uint8_t  incksel3;
	uint8_t  csi_timing;
};

/*
 * "Register List of All-pixel scan mode" (page 49): the three datasheet-
 * supported INCK frequencies (also given in the AC Characteristics INCK
 * table, page 15) and their INCKSEL0..3 / 0x418C settings for All-pixel
 * scan mode, AD = 10 bit. The 74.25 MHz row happens to equal the POR
 * defaults for all five bytes -- written explicitly anyway so the same
 * code path covers all three INCKs uniformly.
 */
static const struct imx296_inck_regs imx296_inck_table[] = {
	{ .hz         = 37125000,
	  .incksel0   = 0x80,
	  .incksel1   = 0x0B,
	  .incksel2   = 0x80,
	  .incksel3   = 0x08,
	  .csi_timing = 0x74 },
	{ .hz         = 54000000,
	  .incksel0   = 0xB0,
	  .incksel1   = 0x0F,
	  .incksel2   = 0xB0,
	  .incksel3   = 0x0C,
	  .csi_timing = 0xA8 },
	{ .hz         = 74250000,
	  .incksel0   = 0x80,
	  .incksel1   = 0x0F,
	  .incksel2   = 0x80,
	  .incksel3   = 0x0C,
	  .csi_timing = 0xE8 },
};

/*
 * "Image Data Output Format" (page 45): CSI-2 RAW10 data type (0x2B).
 *
 * Bayer order: NOT taken from the "Color Coding Diagram" (page 22) -- that
 * diagram gives the physical colour-filter phase at the TOTAL pixel array's
 * outer edge (adjacent to the N1/A1 pins), which is a different row than the
 * first transmitted RAW10 row (readout starts at the OB side, not the N1
 * side -- see the IMX296_WIDTH/IMX296_HEIGHT comment above).
 * The authoritative source is the "Register List of All-pixel scan mode" ->
 * "Pixel Array Image Drawing in All-pixel scan Mode" / "Drive Timing Chart
 * for Serial Output in All-pixel Scan Mode" (page 50), which draws the CFA
 * swatch at the colour-processing margin's own top-left corner (the first
 * transmitted RAW10 line, first transmitted column, HREVERSE/VREVERSE at
 * their POR "Normal" default of 0, matching this driver -- it never touches
 * IMX296_REG_REVERSE): that swatch reads R,G on the first row and G,B on the
 * second, i.e. RGGB in V4L2/Zephyr Bayer naming, not GBRG. The margins are
 * even (8 columns, 4 rows), so the Recording pixel area inside starts on the
 * same RGGB phase -- the page draws the same swatch there too.
 *
 * NOT YET SILICON-VERIFIED: bench run 229 (this driver's only bench pass so
 * far) confirmed the module's IDENTITY (SENSOR_INFO, colour LQR variant)
 * only -- it did not stream a frame, so the RGGB phase above is still the
 * datasheet-derived claim it always was, not a captured/demosaiced-image
 * confirmation (contrast docs/camera-shields.md's OV5647 section, run 62,
 * where the Bayer order WAS confirmed against a rendered frame).
 */
static const struct video_format_cap imx296_fmts[] = {
	{
	    .pixelformat = VIDEO_PIX_FMT_SRGGB10P,
	    .width_min   = IMX296_WIDTH,
	    .width_max   = IMX296_WIDTH,
	    .width_step  = 1,
	    .height_min  = IMX296_HEIGHT,
	    .height_max  = IMX296_HEIGHT,
	    .height_step = 1,
	},
	/*
	 * Centred ROI crop (issue #2287 Stage B, IMX296_ROI_WIDTH/HEIGHT's own comment above).
	 * IMX296_ROI_POS_H/V (88, 64) are both even, so the crop's top-left pixel sits on the
	 * same RGGB phase as the full-frame mode above -- the Bayer-order comment's "even
	 * margins keep the same phase" reasoning applies to this offset too, not just the
	 * sensor's own fixed colour-processing margin.
	 */
	{
	    .pixelformat = VIDEO_PIX_FMT_SRGGB10P,
	    .width_min   = IMX296_ROI_WIDTH,
	    .width_max   = IMX296_ROI_WIDTH,
	    .width_step  = 1,
	    .height_min  = IMX296_ROI_HEIGHT,
	    .height_max  = IMX296_ROI_HEIGHT,
	    .height_step = 1,
	},
	{ 0 },
};

static const int64_t imx296_link_frequencies[] = {
	IMX296_LINK_FREQ_HZ,
};

struct imx296_ctrls {
	struct video_ctrl exposure;
	struct video_ctrl analogue_gain;
	struct video_ctrl hflip;
	struct video_ctrl vflip;
	struct video_ctrl pixel_rate;
	struct video_ctrl link_freq;
	struct video_ctrl trigger_mode;
};

struct imx296_data {
	struct imx296_ctrls ctrls;
	struct video_format fmt;
	/* Set by imx296_set_stream(dev, true, ...) once STANDBY is cancelled, cleared by
	 * ...(dev, false, ...) once STANDBY is re-armed. Zero-initialised to false at boot (this
	 * struct is a static DEVICE_DT_INST_DEFINE instance, never heap-allocated) -- imx296_init()
	 * itself never touches this field, it just never leaves the sensor other than parked in
	 * STANDBY. See IMX296_CID_TRIGGER_MODE's set_ctrl case for why this gates it. */
	bool streaming;
};

struct imx296_config {
	struct i2c_dt_spec i2c;
	uint32_t           input_clk_hz;
};

static const struct imx296_inck_regs *imx296_find_inck(uint32_t input_clk_hz)
{
	for (size_t i = 0; i < ARRAY_SIZE(imx296_inck_table); i++) {
		if (imx296_inck_table[i].hz == input_clk_hz) {
			return &imx296_inck_table[i];
		}
	}

	return NULL;
}

static int imx296_set_fmt(const struct device *dev, struct video_format *fmt)
{
	struct imx296_data *data = dev->data;
	size_t              idx;
	int                 ret;

	ret = video_format_caps_index(imx296_fmts, fmt, &idx);
	if (ret < 0) {
		LOG_ERR("Format '%s' %ux%u not supported",
		        VIDEO_FOURCC_TO_STR(fmt->pixelformat),
		        fmt->width,
		        fmt->height);
		return -ENOTSUP;
	}

	data->fmt = *fmt;

	return 0;
}

static int imx296_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct imx296_data *data = dev->data;

	*fmt = data->fmt;

	return 0;
}

static int imx296_get_caps(const struct device *dev, struct video_caps *caps)
{
	if (caps->type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	caps->format_caps = imx296_fmts;

	return 0;
}

static int imx296_enum_frmival(const struct device *dev, struct video_frmival_enum *fie)
{
	size_t idx;
	int    ret;

	ret = video_format_caps_index(imx296_fmts, fie->format, &idx);
	if (ret < 0 || fie->index != 0) {
		return -EINVAL;
	}

	fie->type                 = VIDEO_FRMIVAL_TYPE_DISCRETE;
	fie->discrete.numerator   = IMX296_FRMIVAL_NUMERATOR;
	fie->discrete.denominator = IMX296_FRMIVAL_DENOMINATOR;

	return 0;
}

static int imx296_set_frmival(const struct device *dev, struct video_frmival *frmival)
{
	/* All-pixel scan mode has exactly one fixed frame rate (60.3 frame/s) */
	frmival->numerator   = IMX296_FRMIVAL_NUMERATOR;
	frmival->denominator = IMX296_FRMIVAL_DENOMINATOR;

	return 0;
}

static int imx296_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	frmival->numerator   = IMX296_FRMIVAL_NUMERATOR;
	frmival->denominator = IMX296_FRMIVAL_DENOMINATOR;

	return 0;
}

static int imx296_set_stream(const struct device *dev, bool on, enum video_buf_type type)
{
	const struct imx296_config *cfg  = dev->config;
	struct imx296_data         *data = dev->data;
	int                         ret;

	if (type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	if (on) {
		bool trigger = data->ctrls.trigger_mode.val != IMX296_TRIGGER_MODE_FREE_RUN;

		/*
		 * "Mode Transitions of Global Shutter Operation" (page 66): "In case of Fast
		 * Trigger mode, the mode transition must be done via sensor standby." Both
		 * TRIGEN and LOWLAGTRG (and SYNCSEL) reflect "S" (standby-set, see their own
		 * comments above), so writing them here -- before STANDBY is cancelled below --
		 * is what makes the mode take effect on this stream-start. The sensor is always
		 * parked in STANDBY at this point: imx296_init() leaves it there, and the stop
		 * path at the end of this function re-arms it.
		 */
		ret =
		    video_write_cci_reg(&cfg->i2c, IMX296_REG_TRIGEN, trigger ? IMX296_TRIGEN_TRIGGER : 0);
		if (ret < 0) {
			return ret;
		}

		ret = video_write_cci_reg(
		    &cfg->i2c, IMX296_REG_LOWLAGTRG, trigger ? IMX296_LOWLAGTRG_FAST : 0);
		if (ret < 0) {
			return ret;
		}

		ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_SYNCSEL, IMX296_SYNCSEL_NORMAL);
		if (ret < 0) {
			return ret;
		}

		/*
		 * ROI mode (issue #2287 Stage B, IMX296_REG_ROI_ENABLE's own comment above):
		 * `data->fmt` is whatever video_set_format() last accepted from imx296_fmts[]
		 * (imx296_set_fmt() only lets the two entries there through), so this reads it
		 * back rather than tracking a separate "ROI requested" flag. Like TRIGEN/
		 * LOWLAGTRG/SYNCSEL just above, these land here -- while still in standby, before
		 * STANDBY is cancelled below -- for the same reason: whatever their own reflect
		 * timing ("V"/"I", not "S" -- see IMX296_REG_ROI_POS_H's comment above), a value
		 * written while the sensor's clocks are still halted in standby cannot land
		 * mid-frame, so this is a safe common write window regardless of reflect type.
		 * The position/size registers are written UNCONDITIONALLY to the one supported
		 * crop whenever ROI mode is selected -- there is no second ROI geometry to choose
		 * between, so nothing here needs to persist across a full-frame stream start.
		 *
		 * "ROI mode" (page 51): "One invalid frame is generated when the ROI area
		 * changing size or cropping address." This driver never leaves the sensor
		 * streaming while switching between the full-frame and ROI formats --
		 * video_set_format() (imx296_set_fmt()) only updates `data->fmt`, and these
		 * writes only reach hardware from THIS function, on the next stream start, with
		 * the sensor already in STANDBY -- so any ROI-vs-full-frame switch is always a
		 * stop/change-format/start sequence, never a live switch while streaming. The
		 * one-invalid-frame cost this note warns about is covered by the SAME
		 * IMX296_INIT_PERIOD_MS wait below that already covers the sensor's normal
		 * post-STANDBY-cancel init period (150 ms, 9 frames of margin -- comfortably
		 * more than the single frame this note describes), not a separate wait.
		 */
		if (data->fmt.width == IMX296_ROI_WIDTH && data->fmt.height == IMX296_ROI_HEIGHT) {
			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_POS_H, IMX296_ROI_POS_H);
			if (ret < 0) {
				return ret;
			}

			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_POS_V, IMX296_ROI_POS_V);
			if (ret < 0) {
				return ret;
			}

			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_SIZE_H, IMX296_ROI_WIDTH);
			if (ret < 0) {
				return ret;
			}

			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_SIZE_V, IMX296_ROI_HEIGHT);
			if (ret < 0) {
				return ret;
			}

			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_ENABLE, IMX296_ROI_ENABLE_ENABLE);
		} else {
			ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_ENABLE, IMX296_ROI_ENABLE_DISABLE);
		}
		if (ret < 0) {
			return ret;
		}

		/* "Standby mode" (page 54): cancel standby, then wait for regulator settling */
		ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, 0);
		if (ret < 0) {
			return ret;
		}

		/*
		 * Mark streaming (i.e. "no longer in standby, TRIGEN/LOWLAGTRG can no longer be
		 * changed") the moment STANDBY is actually cancelled above, NOT after the XMSTA
		 * write below succeeds. If XMSTA's write itself failed here (e.g. I2C NAK), the
		 * sensor would already be out of standby with no clean way back into it from this
		 * function's own error return -- leaving data->streaming false in that case would
		 * let IMX296_CID_TRIGGER_MODE's set_ctrl case accept a mode change that then has
		 * no standby window left to land in when a caller next tries to stream.
		 */
		data->streaming = true;

		k_sleep(K_MSEC(IMX296_STANDBY_SETTLE_MS));

		/*
		 * "Slave Mode and Master Mode" (page 55): start master-mode free-run. Fast
		 * trigger mode also "supports Master mode only" (page 64) -- the same XMSTA
		 * start applies to both trigger and free-run streaming, only TRIGEN/LOWLAGTRG
		 * above change which one XTRIG then drives.
		 */
		ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_XMSTA, 0);
		if (ret < 0) {
			return ret;
		}

		/*
		 * "Standby mode" (page 54): wait out the 8-frame initialization period
		 * before returning -- see IMX296_INIT_PERIOD_MS's comment above for why
		 * this blocking wait, not a caller-side delay, is what keeps the CPI from
		 * ever capturing one of these invalid frames.
		 */
		k_sleep(K_MSEC(IMX296_INIT_PERIOD_MS));

		return 0;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_XMSTA, IMX296_XMSTA_STOP);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, IMX296_STANDBY_STANDBY);
	if (ret < 0) {
		return ret;
	}

	data->streaming = false;

	return 0;
}

static int imx296_set_ctrl(const struct device *dev, uint32_t cid)
{
	const struct imx296_config *cfg   = dev->config;
	struct imx296_data         *data  = dev->data;
	struct imx296_ctrls        *ctrls = &data->ctrls;
	uint32_t                    shs;

	switch (cid) {
	case VIDEO_CID_EXPOSURE:
		/*
		 * "Calculation Formula of Exposure Time" (page 60): the exposure control here
		 * is in "lines of integration" (lines_per_frame - SHS), ascending = more
		 * exposure, converted to the inversely-related SHS register value on write.
		 */
		shs = IMX296_VMAX - ctrls->exposure.val;
		return video_write_cci_reg(&cfg->i2c, IMX296_REG_SHS, shs);
	case VIDEO_CID_ANALOGUE_GAIN:
		return video_write_cci_reg(&cfg->i2c, IMX296_REG_GAIN, ctrls->analogue_gain.val);
	case VIDEO_CID_HFLIP:
		return video_modify_cci_reg(&cfg->i2c,
		                            IMX296_REG_REVERSE,
		                            IMX296_REVERSE_HREVERSE,
		                            ctrls->hflip.val != 0 ? IMX296_REVERSE_HREVERSE : 0);
	case VIDEO_CID_VFLIP:
		return video_modify_cci_reg(&cfg->i2c,
		                            IMX296_REG_REVERSE,
		                            IMX296_REVERSE_VREVERSE,
		                            ctrls->vflip.val != 0 ? IMX296_REVERSE_VREVERSE : 0);
	case IMX296_CID_TRIGGER_MODE:
		/*
		 * "Mode Transitions of Global Shutter Operation" (page 66): the switch can only
		 * be made "via sensor standby" -- while streaming there is no standby to make it
		 * through, so reject outright rather than silently queuing the change for
		 * whatever stream-stop/start cycle happens to come next. video_set_ctrl() backs
		 * up and restores ctrls->trigger_mode.val on a non-zero return (video_ctrls.c),
		 * so returning -EBUSY here also undoes the core's already-applied write --
		 * nothing is left half-changed.
		 */
		if (data->streaming) {
			return -EBUSY;
		}

		/*
		 * No immediate register write otherwise: TRIGEN/LOWLAGTRG are only acted on by
		 * imx296_set_stream() the next time streaming starts (see IMX296_REG_TRIGEN's
		 * comment).
		 */
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(video, imx296_driver_api) = {
	.set_format   = imx296_set_fmt,
	.get_format   = imx296_get_fmt,
	.get_caps     = imx296_get_caps,
	.set_stream   = imx296_set_stream,
	.set_ctrl     = imx296_set_ctrl,
	.set_frmival  = imx296_set_frmival,
	.get_frmival  = imx296_get_frmival,
	.enum_frmival = imx296_enum_frmival,
};

static int imx296_init_ctrls(const struct device *dev)
{
	struct imx296_data  *data  = dev->data;
	struct imx296_ctrls *ctrls = &data->ctrls;
	int                  ret;

	/*
	 * #2287 Stage B unit 3 (bench runs 304-306, E1M-AEN803 2026W36-0001; wording corrected
	 * after run 310): .max is IMX296_VMAX - IMX296_SHS_DEFAULT (VMAX - 14 = 1104), NOT
	 * IMX296_VMAX - IMX296_SHS_MIN (VMAX - 4 = 1114) -- the datasheet's own legal SHS floor
	 * (p.60's "Register List of Shutter setting", 4 <= SHS <= VMAX - 1) says SHS=4 is valid,
	 * but bench evidence is it isn't usable AT THIS DRIVER'S OWN ROI TIMING: run 304 (SHS=4,
	 * gain 0, ROI mode) produced a completely flat, black frame with no scene content, while
	 * run 306 (SHS=14, same ROI mode/timing) produced a real (if still imperfect) scene. This
	 * is NOT unexplained: the ROI mode's own frame-rate/lines-per-frame table (p.53/p.61)
	 * tabulates lines-per-frame as VTR = ROIWV1 + 30 = 990 (for this driver's ROIWV1 = 960)
	 * -- but this driver deliberately keeps VMAX at its all-pixel-scan-mode value, 1118, in
	 * ROI mode too (see the ROI-mode block's own comment above, "VMAX stays >= ROIWV1 + 30")
	 * rather than switching to the datasheet's own tabulated 990. Run 304's SHS=4 failure was
	 * therefore observed in a lines-per-frame configuration the datasheet never itself
	 * tabulates SHS=4 against -- 1104 is a bench-proven WORKAROUND for THIS untabulated
	 * VMAX=1118 ROI configuration, not an isolated root cause; gain is separately ruled out
	 * (run 304 held gain at 0). IMX296_SHS_MIN itself is UNCHANGED (still 4, the datasheet's
	 * own legal floor at its OWN tabulated ROI timing, VTR=990, which this driver does not
	 * currently use) so a future fix that switches ROI mode to the tabulated VTR only needs to
	 * widen this .max back to IMX296_VMAX - IMX296_SHS_MIN, not touch the constant itself.
	 */
	ret = video_init_ctrl(&ctrls->exposure,
	                      dev,
	                      VIDEO_CID_EXPOSURE,
	                      (struct video_ctrl_range){ .min  = 1,
	                                                 .max  = IMX296_VMAX - IMX296_SHS_DEFAULT,
	                                                 .step = 1,
	                                                 .def  = IMX296_VMAX - IMX296_SHS_DEFAULT });
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(
	    &ctrls->analogue_gain,
	    dev,
	    VIDEO_CID_ANALOGUE_GAIN,
	    (struct video_ctrl_range){ .min = 0, .max = IMX296_GAIN_MAX, .step = 1, .def = 0 });
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

	ret = video_init_ctrl(&ctrls->pixel_rate,
	                      dev,
	                      VIDEO_CID_PIXEL_RATE,
	                      (struct video_ctrl_range){ .min64  = IMX296_PIXEL_RATE_HZ,
	                                                 .max64  = IMX296_PIXEL_RATE_HZ,
	                                                 .step64 = 1,
	                                                 .def64  = IMX296_PIXEL_RATE_HZ });
	if (ret < 0) {
		return ret;
	}
	ctrls->pixel_rate.flags |= VIDEO_CTRL_FLAG_READ_ONLY;

	ret = video_init_int_menu_ctrl(&ctrls->link_freq,
	                               dev,
	                               VIDEO_CID_LINK_FREQ,
	                               0,
	                               imx296_link_frequencies,
	                               ARRAY_SIZE(imx296_link_frequencies));
	if (ret < 0) {
		return ret;
	}
	ctrls->link_freq.flags |= VIDEO_CTRL_FLAG_READ_ONLY;

	/* IMX296_CID_TRIGGER_MODE's comment above: default free-run, unchanged behaviour. */
	ret = video_init_ctrl(&ctrls->trigger_mode,
	                      dev,
	                      IMX296_CID_TRIGGER_MODE,
	                      (struct video_ctrl_range){ .min  = IMX296_TRIGGER_MODE_FREE_RUN,
	                                                 .max  = IMX296_TRIGGER_MODE_EXTERNAL,
	                                                 .step = 1,
	                                                 .def  = IMX296_TRIGGER_MODE_FREE_RUN });
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int imx296_init(const struct device *dev)
{
	const struct imx296_config    *cfg = dev->config;
	const struct imx296_inck_regs *inck;
	struct video_format            fmt = {
		.pixelformat = VIDEO_PIX_FMT_SRGGB10P,
		.width       = IMX296_WIDTH,
		.height      = IMX296_HEIGHT,
	};
	uint32_t standby;
	uint32_t sensor_info;
	int      ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	/*
	 * Log-only connectivity + cold/warm-boot read: the RPi-style J5 connector this part sits
	 * behind carries no reset line, and the module stays powered across a SoC warm
	 * reset/reflash, so STANDBY can legitimately read back 0 (already running) on any init
	 * after the first. video_read_cci_reg() already returns a negative errno on I2C NAK/
	 * timeout, so this also doubles as an early-exit connectivity check before the
	 * SENSOR_INFO identity probe below does the real gating.
	 */
	ret = video_read_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, &standby);
	if (ret < 0) {
		return ret;
	}

	LOG_DBG("STANDBY read back 0x%02x (%s boot)",
	        standby,
	        (standby & IMX296_STANDBY_STANDBY) != 0 ? "cold" : "warm");

	inck = imx296_find_inck(cfg->input_clk_hz);
	if (inck == NULL) {
		LOG_ERR("Unsupported INCK frequency %u Hz (must be 37.125/54/74.25 MHz)",
		        cfg->input_clk_hz);
		return -ENOTSUP;
	}

	/*
	 * Make init idempotent regardless of the sensor's prior state (POR, or already
	 * free-running from before a warm SoC reset): "Slave Mode and Master Mode" (page 55)
	 * + "Standby mode" (page 54) give XMSTA-stop-then-STANDBY as the documented order to
	 * quiesce master-mode streaming before touching the "S" (standby-only) registers below
	 * -- the same order imx296_set_stream(dev, false, ...) uses to stop an active stream.
	 */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_XMSTA, IMX296_XMSTA_STOP);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, IMX296_STANDBY_STANDBY);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Chip-ID: cancel standby just long enough to read SENSOR_INFO (see the register
	 * comment above -- bench run 229 read it only after cancelling standby, so this driver
	 * does not assume it is readable while parked) and reject any silicon that does not
	 * match the one bench-confirmed colour-IMX296LQR-C signature. Re-parks in standby
	 * immediately after: the VMAX/HMAX/INCKSEL/CSI_TIMING writes below are "S" (standby-
	 * only) registers.
	 */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, 0);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(IMX296_STANDBY_SETTLE_MS));

	ret = video_read_cci_reg(&cfg->i2c, IMX296_REG_SENSOR_INFO, &sensor_info);
	if (ret < 0) {
		return ret;
	}

	if (sensor_info != IMX296_SENSOR_INFO_LQR_COLOUR) {
		LOG_ERR("SENSOR_INFO read 0x%04x, expected the bench-confirmed colour "
		        "IMX296LQR-C signature 0x%04x -- wrong/unrecognised silicon, or a mono "
		        "IMX296LLR module this driver does not support (its fixed SRGGB10P "
		        "format targets the colour variant only)",
		        sensor_info,
		        IMX296_SENSOR_INFO_LQR_COLOUR);
		return -ENODEV;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, IMX296_STANDBY_STANDBY);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_VMAX, IMX296_VMAX);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_HMAX, IMX296_HMAX);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_INCKSEL0, inck->incksel0);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_INCKSEL1, inck->incksel1);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_INCKSEL2, inck->incksel2);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_INCKSEL3, inck->incksel3);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_CSI_TIMING, inck->csi_timing);
	if (ret < 0) {
		return ret;
	}

	/* See IMX296_REG_CSI_LANE_HS's own comment above: bench-derived, no datasheet coverage. */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_CSI_LANE_HS, IMX296_CSI_LANE_HS_VAL);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_BLKLEVEL, IMX296_BLKLEVEL);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Force All-pixel scan mode (not ROI mode) regardless of what a prior session left this
	 * register -- see IMX296_REG_ROI_ENABLE's own comment above.
	 */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_ROI_ENABLE, IMX296_ROI_ENABLE_DISABLE);
	if (ret < 0) {
		return ret;
	}

	/*
	 * imx296_init_ctrls() below only creates the v4.4 video-control-registry
	 * entries with their default VALUES -- it never touches hardware. On a
	 * cold boot the sensor's own POR defaults already agree with these (SHS
	 * POR default matches IMX296_SHS_DEFAULT, GAIN POR default is 0, REVERSE
	 * POR default is 0 -- see each register's own comment above), but on a
	 * warm SoC reset (this module stays powered across one, see the STANDBY
	 * comment above imx296_init()) the sensor can already carry a PRIOR
	 * session's SHS/GAIN/REVERSE values while the control registry resets to
	 * its defaults -- silently disagreeing with hardware until the next
	 * explicit video_set_ctrl() call. Write all three here so control cache
	 * and hardware start in agreement regardless of boot history.
	 */
	/*
	 * SHS is the shutter start line; IMX296_SHS_DEFAULT (14) is already the
	 * register value, giving the exposure control's default of VMAX - 14 =
	 * 1104 lines (issue #2287, see changelog.d/2287.md for the bug history).
	 */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_SHS, IMX296_SHS_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_GAIN, 0);
	if (ret < 0) {
		return ret;
	}

	/*
	 * #2287 (bench runs 304-306): GAINDLY's own POR default (00h) is a datasheet-prohibited
	 * value (see IMX296_REG_GAINDLY's own comment) -- write the one legal value this driver
	 * uses (09h, "Gain reflect at the next frame (Same timing as SHS reflecting output.)",
	 * page 41 verbatim) unconditionally at init, same "hardware and control cache start in
	 * agreement" reasoning as SHS/GAIN/REVERSE above (GAINDLY has no v4.4 video-control-
	 * registry entry of its own -- it's a fixed hardware setting, not something an app ever
	 * changes via video_set_ctrl() -- so there is no separate "warm reset disagreement" case
	 * to cover, only ensuring it is never left at the prohibited POR value).
	 */
	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_GAINDLY, IMX296_GAINDLY_DELAY1);
	if (ret < 0) {
		return ret;
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_REVERSE, 0);
	if (ret < 0) {
		return ret;
	}

	ret = imx296_set_fmt(dev, &fmt);
	if (ret < 0) {
		return ret;
	}

	return imx296_init_ctrls(dev);
}

#define IMX296_EP(n)        DT_CHILD(DT_INST_CHILD(n, port), endpoint)
#define IMX296_INPUT_CLK(n) DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency)

#define IMX296_INIT(n) \
	BUILD_ASSERT(DT_PROP_OR(IMX296_EP(n), bus_type, VIDEO_BUS_TYPE_CSI2_DPHY) == \
	                 VIDEO_BUS_TYPE_CSI2_DPHY, \
	             "Only the MIPI CSI-2 D-PHY interface is supported"); \
	BUILD_ASSERT(DT_PROP_LEN_OR(IMX296_EP(n), data_lanes, 1) == 1, \
	             "IMX296 has a single MIPI CSI-2 data lane"); \
\
	static struct imx296_data imx296_data_##n; \
\
	static const struct imx296_config imx296_cfg_##n = { \
		.i2c          = I2C_DT_SPEC_INST_GET(n), \
		.input_clk_hz = IMX296_INPUT_CLK(n), \
	}; \
\
	DEVICE_DT_INST_DEFINE(n, \
	                      &imx296_init, \
	                      NULL, \
	                      &imx296_data_##n, \
	                      &imx296_cfg_##n, \
	                      POST_KERNEL, \
	                      CONFIG_VIDEO_INIT_PRIORITY, \
	                      &imx296_driver_api); \
\
	VIDEO_DEVICE_DEFINE(imx296_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(IMX296_INIT)
