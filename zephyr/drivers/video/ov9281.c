/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (third-party permissive port), BENCH-VERIFIED ======
 * OmniVision OV9281 global-shutter mono MIPI CSI-2 sensor. Ported onto the
 * upstream Zephyr v4.4 video API (video_driver_api set_format/get_format/
 * get_caps/set_stream + the video_ctrls registry) from the Apache-2.0
 * Espressif esp-video-components esp_cam_sensor/sensors/ov9281 driver
 * (esp_cam_sensor/sensors/ov9281/{ov9281.c, private_include/ov9281_regs.h,
 * private_include/ov9281_settings.h,
 * private_include/ov9281_mipi_2lane_24Minput_1280x720_raw8_50fps.h,
 * private_include/ov9281_mipi_2lane_24Minput_640x400_raw8_100fps.h}) @ commit
 * f1e4bff773ce36204434a47bdefb5bc943dc6647 (repo espressif/esp-video-components,
 * path esp_cam_sensor/sensors/ov9281).
 *
 * The register addresses, values and per-mode init tables are the consumed
 * Espressif asset and are kept byte-for-byte (see ov9281_mode_640x400_100fps
 * / ov9281_mode_1280x720_50fps below); everything around them -- the
 * esp_cam_sensor_ops_t / SCCB-driver-handle skeleton, the ESP-IDF FreeRTOS
 * glue, and the ESP-specific Kconfig knobs (CONFIG_CAMERA_OV9281_*) -- does
 * not carry over to Zephyr and was rewritten against
 * zephyr/drivers/video/imx219.c's v4.4 shape (DT_DRV_COMPAT, i2c_dt_spec,
 * the video_driver_api table, video_ctrl registry, CCI helpers from
 * video_common.h). Two of the three supported modes are the Espressif ones
 * -- 1280x720 RAW8 @ 50 fps and 640x400 RAW8 @ 100 fps; the third, the
 * sensor's full 1280x800 active array (also RAW8/GREY), is Alp-authored (see
 * the note below and the derivation comment on
 * ov9281_mode_1280x800_100fps_regs). All three run 2 MIPI data lanes,
 * 24 MHz XVCLK -- no RAW10 or other pixel format is invented here.
 *
 * BENCH-VERIFIED 2026-09-21 on an E1M-AEN803 on the E1M-EVK
 * (innomaker_cam_ov9281 shield on J5): live GREY8 frames captured and
 * CRC-verified via an SWD dump in all three modes (640x400, 1280x720,
 * 1280x800). The discriminating evidence is a 0xA5-prefilled pool being
 * overwritten plus the sensor's own test pattern appearing on request --
 * not merely a non-zero CRC (a warm RAM-run can leave a previous frame in
 * SRAM0, so a non-zero buffer alone proves nothing). The test pattern was
 * verified in all three modes (pure 0x00/0xFF bars, no other values). A
 * 60-frame wall-clock burst per mode measured 640x400 ~100 fps (590 ms),
 * 1280x720 ~50 fps (1169 ms) and 1280x800 ~100 fps (594 ms) -- every mode
 * runs at its configured rate, and the bursts themselves bench-prove the
 * buffer-starvation pause/resume fix (video_alif.c, #226): a burst longer
 * than one buffer's worth of consumer latency depends on it. The camera
 * connector on this SoM/EVK combination needs a P/N-crossing adapter (see
 * docs/boards/e1m-evk.md's Camera section) -- without one, the sensor still
 * answers its I2C probe but no frame ever arrives.
 *
 * A third mode, 1280x800 GREY (RAW8) -- the OV9281's full 1280x800 active
 * array -- is Alp-authored, not Espressif's: Espressif upstream ships only
 * 640x400@100 and 1280x720@50. ov9281_mode_1280x800_100fps_regs below is
 * derived from ov9281_mode_1280x720_50fps_regs by changing only the Y-window
 * and VTS registers (see the comment above that table); BENCH-VERIFIED
 * 2026-09-21 -- the 100 fps figure was a same-line-rate projection and now
 * matches the measured wall-clock rate above.
 *
 * RETIREMENT: this is a ported third-party permissive driver, not a Tier-2
 * fork-driver copy or an interim backport -- it has no upstream Zephyr
 * equivalent to retire onto. If upstream Zephyr ever grows a native
 * "ovti,ov9281" driver, prefer it and delete this file. See the "Amendment
 * (2026-09-18)" section of docs/adr/0017-alp-sdk-over-the-vendor-sdk.md for
 * why this is labeled Tier-1.5 rather than Tier-2: Tier-2 in this ladder
 * names the opt-in Alif vendor-SDK fork specifically, which this driver
 * never touches.
 * ======================================================================
 */

#define DT_DRV_COMPAT ovti_ov9281

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

LOG_MODULE_REGISTER(ov9281, CONFIG_VIDEO_LOG_LEVEL);

/* Espressif esp_cam_sensor/sensors/ov9281/private_include/ov9281_regs.h */
#define OV9281_CHIP_ID			0x9281

#define OV9281_REG8(addr)		((addr) | VIDEO_REG_ADDR16_DATA8)
#define OV9281_REG16(addr)		((addr) | VIDEO_REG_ADDR16_DATA16_BE)

#define OV9281_REG_CHIP_ID		OV9281_REG16(0x300a)
#define OV9281_REG_SW_RESET		OV9281_REG8(0x0103)
#define OV9281_REG_CTRL_MODE		OV9281_REG8(0x0100)
#define OV9281_MODE_SW_STANDBY		0x00
#define OV9281_MODE_STREAMING		0x01
#define OV9281_REG_EXPOSURE_H		OV9281_REG8(0x3500)
#define OV9281_REG_EXPOSURE_M		OV9281_REG8(0x3501)
#define OV9281_REG_EXPOSURE_L		OV9281_REG8(0x3502)
#define OV9281_REG_GAIN			OV9281_REG8(0x3509)
#define OV9281_REG_TEST_PATTERN		OV9281_REG8(0x5e00)
#define OV9281_TEST_PATTERN_ENABLE	BIT(7)

/* 4 least significant bits of the exposure value are its fractional part */
#define OV9281_FETCH_EXP_H(val)	(((val) >> 12) & 0xf)
#define OV9281_FETCH_EXP_M(val)	(((val) >> 4) & 0xff)
#define OV9281_FETCH_EXP_L(val)	(((val) & 0xf) << 4)

/* Mini exposure time: 1 row period. Max exposure time: VTS - 25 row periods. */
#define OV9281_EXPOSURE_MIN		0x05
#define OV9281_EXPOSURE_DEFAULT		0x2a9
#define OV9281_EXP_MAX_OFFSET		25
#define OV9281_GAIN_DEFAULT		0x18
#define OV9281_GAIN_DEFAULT_INDEX	8
#define OV9281_BLC_TARGET_DEFAULT	0x10

#define OV9281_INPUT_CLK_HZ		MHZ(24)
#define OV9281_LINK_FREQ_HZ		400000000

/* Espressif ov9281_gain_reg_map[]: 112-entry discrete analogue-gain LUT (index -> reg 0x3509) */
static const uint8_t ov9281_gain_reg_map[] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
	0x40, 0x42, 0x44, 0x46, 0x48, 0x4a, 0x4c, 0x4e, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5a, 0x5c, 0x5e,
	0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0x6c, 0x6e, 0x70, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7c, 0x7e,
	0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc,
	0xc0, 0xc4, 0xc8, 0xcc, 0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8, 0xfc,
};

/*
 * Espressif esp_cam_sensor/sensors/ov9281/private_include/
 * ov9281_mipi_2lane_24Minput_640x400_raw8_100fps.h -- MIPI_2lane_24Minput_RAW8_640x400_100fps.
 * Kept verbatim, minus the trailing streaming-enable + end-of-table sentinel entries the
 * Espressif ov9281_reginfo_t format used (set_stream() and ARRAY_SIZE() cover those here).
 */
static const struct video_reg ov9281_mode_640x400_100fps_regs[] = {
	{OV9281_REG8(0x0103), 0x01}, {OV9281_REG8(0x0106), 0x00}, {OV9281_REG8(0x0302), 0x32},
	{OV9281_REG8(0x030d), 0x50}, {OV9281_REG8(0x030e), 0x02}, {OV9281_REG8(0x3001), 0x00},
	{OV9281_REG8(0x3004), 0x00}, {OV9281_REG8(0x3005), 0x00}, {OV9281_REG8(0x3006), 0x04},
	{OV9281_REG8(0x3011), 0x0a}, {OV9281_REG8(0x3013), 0x18}, {OV9281_REG8(0x301c), 0xf0},
	{OV9281_REG8(0x3022), 0x01}, {OV9281_REG8(0x3030), 0x10}, {OV9281_REG8(0x3039), 0x32},
	{OV9281_REG8(0x303a), 0x00}, {OV9281_REG8(0x3500), 0x00}, {OV9281_REG8(0x3501), 0x2a},
	{OV9281_REG8(0x3502), 0x90}, {OV9281_REG8(0x3503), 0x08}, {OV9281_REG8(0x3505), 0x8c},
	{OV9281_REG8(0x3507), 0x03}, {OV9281_REG8(0x3508), 0x00},
	{OV9281_REG8(0x3509), OV9281_GAIN_DEFAULT},
	{OV9281_REG8(0x3610), 0x80}, {OV9281_REG8(0x3611), 0xa0}, {OV9281_REG8(0x3620), 0x6e},
	{OV9281_REG8(0x3632), 0x56}, {OV9281_REG8(0x3633), 0x78}, {OV9281_REG8(0x3662), 0x07},
	{OV9281_REG8(0x3666), 0x00}, {OV9281_REG8(0x366f), 0x5a}, {OV9281_REG8(0x3680), 0x84},
	{OV9281_REG8(0x3707), 0x56}, {OV9281_REG8(0x370d), 0x00}, {OV9281_REG8(0x370e), 0xfa},
	{OV9281_REG8(0x3712), 0x80}, {OV9281_REG8(0x372d), 0x22}, {OV9281_REG8(0x3731), 0x80},
	{OV9281_REG8(0x3732), 0x30}, {OV9281_REG8(0x3778), 0x10}, {OV9281_REG8(0x377d), 0x22},
	{OV9281_REG8(0x3788), 0x02}, {OV9281_REG8(0x3789), 0xa4}, {OV9281_REG8(0x378a), 0x00},
	{OV9281_REG8(0x378b), 0x4a}, {OV9281_REG8(0x3799), 0x20}, {OV9281_REG8(0x379c), 0x01},
	{OV9281_REG8(0x3800), 0x00}, {OV9281_REG8(0x3801), 0x00}, {OV9281_REG8(0x3802), 0x00},
	{OV9281_REG8(0x3803), 0x00}, {OV9281_REG8(0x3804), 0x05}, {OV9281_REG8(0x3805), 0x0f},
	{OV9281_REG8(0x3806), 0x03}, {OV9281_REG8(0x3807), 0x2f}, {OV9281_REG8(0x3808), 0x02},
	{OV9281_REG8(0x3809), 0x80}, {OV9281_REG8(0x380a), 0x01}, {OV9281_REG8(0x380b), 0x90},
	{OV9281_REG8(0x380c), 0x02}, {OV9281_REG8(0x380d), 0xd8}, {OV9281_REG8(0x380e), 0x04},
	{OV9281_REG8(0x380f), 0x48}, {OV9281_REG8(0x3810), 0x00}, {OV9281_REG8(0x3811), 0x04},
	{OV9281_REG8(0x3812), 0x00}, {OV9281_REG8(0x3813), 0x04}, {OV9281_REG8(0x3814), 0x31},
	{OV9281_REG8(0x3815), 0x22}, {OV9281_REG8(0x3820), 0x60}, {OV9281_REG8(0x3821), 0x01},
	{OV9281_REG8(0x382b), 0x3a}, {OV9281_REG8(0x382c), 0x06}, {OV9281_REG8(0x382d), 0xc2},
	{OV9281_REG8(0x389d), 0x00}, {OV9281_REG8(0x3881), 0x42}, {OV9281_REG8(0x3882), 0x02},
	{OV9281_REG8(0x3883), 0x12}, {OV9281_REG8(0x3885), 0x07}, {OV9281_REG8(0x38a8), 0x02},
	{OV9281_REG8(0x38a9), 0x80}, {OV9281_REG8(0x38b1), 0x03}, {OV9281_REG8(0x38b3), 0x07},
	{OV9281_REG8(0x38c4), 0x00}, {OV9281_REG8(0x38c5), 0xc0}, {OV9281_REG8(0x38c6), 0x04},
	{OV9281_REG8(0x38c7), 0x80}, {OV9281_REG8(0x3920), 0xff},
	{OV9281_REG8(0x4003), OV9281_BLC_TARGET_DEFAULT},
	{OV9281_REG8(0x4008), 0x02}, {OV9281_REG8(0x4009), 0x05}, {OV9281_REG8(0x400c), 0x01},
	{OV9281_REG8(0x400d), 0x03}, {OV9281_REG8(0x4010), 0xf0}, {OV9281_REG8(0x4011), 0x3b},
	{OV9281_REG8(0x4042), 0x01}, {OV9281_REG8(0x4043), 0x40}, {OV9281_REG8(0x4307), 0x30},
	{OV9281_REG8(0x4317), 0x00}, {OV9281_REG8(0x4501), 0x00}, {OV9281_REG8(0x4507), 0x03},
	{OV9281_REG8(0x4509), 0x80}, {OV9281_REG8(0x450a), 0x08}, {OV9281_REG8(0x4601), 0x30},
	{OV9281_REG8(0x470f), 0x00}, {OV9281_REG8(0x4f07), 0x00}, {OV9281_REG8(0x4800), 0x60},
	{OV9281_REG8(0x4837), 0x14}, {OV9281_REG8(0x5000), 0x9f}, {OV9281_REG8(0x5001), 0x00},
	{OV9281_REG8(0x5e00), 0x00}, {OV9281_REG8(0x5d00), 0x07}, {OV9281_REG8(0x5d01), 0x00},
	{OV9281_REG8(0x4f00), 0x0c}, {OV9281_REG8(0x4f10), 0x00}, {OV9281_REG8(0x4f11), 0x88},
	{OV9281_REG8(0x4f12), 0x0f}, {OV9281_REG8(0x4f13), 0xc4},
};

/*
 * Espressif esp_cam_sensor/sensors/ov9281/private_include/
 * ov9281_mipi_2lane_24Minput_1280x720_raw8_50fps.h -- MCLK24M_1280x720_MIPI_2LANE_800Mbps_RAW8_LINEAR_50fps.
 * Kept verbatim (see note above the 640x400 table).
 */
static const struct video_reg ov9281_mode_1280x720_50fps_regs[] = {
	{OV9281_REG8(0x0103), 0x01}, {OV9281_REG8(0x0106), 0x00}, {OV9281_REG8(0x0302), 0x32},
	{OV9281_REG8(0x030d), 0x50}, {OV9281_REG8(0x030e), 0x02}, {OV9281_REG8(0x3001), 0x00},
	{OV9281_REG8(0x3004), 0x00}, {OV9281_REG8(0x3005), 0x00}, {OV9281_REG8(0x3006), 0x04},
	{OV9281_REG8(0x3011), 0x0a}, {OV9281_REG8(0x3013), 0x18}, {OV9281_REG8(0x301c), 0xf0},
	{OV9281_REG8(0x3022), 0x01}, {OV9281_REG8(0x3030), 0x10}, {OV9281_REG8(0x3039), 0x32},
	{OV9281_REG8(0x303a), 0x00}, {OV9281_REG8(0x3500), 0x00}, {OV9281_REG8(0x3501), 0x2a},
	{OV9281_REG8(0x3502), 0x90}, {OV9281_REG8(0x3503), 0x08}, {OV9281_REG8(0x3505), 0x8c},
	{OV9281_REG8(0x3507), 0x03}, {OV9281_REG8(0x3508), 0x00},
	{OV9281_REG8(0x3509), OV9281_GAIN_DEFAULT},
	{OV9281_REG8(0x3610), 0x80}, {OV9281_REG8(0x3611), 0xa0}, {OV9281_REG8(0x3620), 0x6e},
	{OV9281_REG8(0x3632), 0x56}, {OV9281_REG8(0x3633), 0x78}, {OV9281_REG8(0x3662), 0x07},
	{OV9281_REG8(0x3666), 0x00}, {OV9281_REG8(0x366f), 0x5a}, {OV9281_REG8(0x3680), 0x84},
	{OV9281_REG8(0x3707), 0x56}, {OV9281_REG8(0x370d), 0x00}, {OV9281_REG8(0x370e), 0xfa},
	{OV9281_REG8(0x3712), 0x80}, {OV9281_REG8(0x372d), 0x22}, {OV9281_REG8(0x3731), 0x80},
	{OV9281_REG8(0x3732), 0x30}, {OV9281_REG8(0x3778), 0x00}, {OV9281_REG8(0x377d), 0x22},
	{OV9281_REG8(0x3788), 0x02}, {OV9281_REG8(0x3789), 0xa4}, {OV9281_REG8(0x378a), 0x00},
	{OV9281_REG8(0x378b), 0x4a}, {OV9281_REG8(0x3799), 0x20}, {OV9281_REG8(0x379c), 0x01},
	{OV9281_REG8(0x3800), 0x00}, {OV9281_REG8(0x3801), 0x00}, {OV9281_REG8(0x3802), 0x00},
	{OV9281_REG8(0x3803), 0x28}, {OV9281_REG8(0x3804), 0x05}, {OV9281_REG8(0x3805), 0x0f},
	{OV9281_REG8(0x3806), 0x03}, {OV9281_REG8(0x3807), 0x07}, {OV9281_REG8(0x3808), 0x05},
	{OV9281_REG8(0x3809), 0x00}, {OV9281_REG8(0x380a), 0x02}, {OV9281_REG8(0x380b), 0xd0},
	{OV9281_REG8(0x380c), 0x03}, {OV9281_REG8(0x380d), 0x69}, {OV9281_REG8(0x380e), 0x07},
	{OV9281_REG8(0x380f), 0x1c}, {OV9281_REG8(0x3810), 0x00}, {OV9281_REG8(0x3811), 0x08},
	{OV9281_REG8(0x3812), 0x00}, {OV9281_REG8(0x3813), 0x08}, {OV9281_REG8(0x3814), 0x11},
	{OV9281_REG8(0x3815), 0x11}, {OV9281_REG8(0x3820), 0x40}, {OV9281_REG8(0x3821), 0x00},
	{OV9281_REG8(0x382b), 0x3a}, {OV9281_REG8(0x382c), 0x06}, {OV9281_REG8(0x382d), 0xc2},
	{OV9281_REG8(0x389d), 0x00}, {OV9281_REG8(0x3881), 0x42}, {OV9281_REG8(0x3882), 0x02},
	{OV9281_REG8(0x3883), 0x12}, {OV9281_REG8(0x3885), 0x07}, {OV9281_REG8(0x38a8), 0x02},
	{OV9281_REG8(0x38a9), 0x80}, {OV9281_REG8(0x38b1), 0x03}, {OV9281_REG8(0x38b3), 0x07},
	{OV9281_REG8(0x38c4), 0x00}, {OV9281_REG8(0x38c5), 0xc0}, {OV9281_REG8(0x38c6), 0x04},
	{OV9281_REG8(0x38c7), 0x80}, {OV9281_REG8(0x3920), 0xff},
	{OV9281_REG8(0x4003), OV9281_BLC_TARGET_DEFAULT},
	{OV9281_REG8(0x4008), 0x04}, {OV9281_REG8(0x4009), 0x0b}, {OV9281_REG8(0x400c), 0x01},
	{OV9281_REG8(0x400d), 0x07}, {OV9281_REG8(0x4010), 0xf0}, {OV9281_REG8(0x4011), 0x3b},
	{OV9281_REG8(0x4042), 0x01}, {OV9281_REG8(0x4043), 0x40}, {OV9281_REG8(0x4307), 0x30},
	{OV9281_REG8(0x4317), 0x00}, {OV9281_REG8(0x4501), 0x00}, {OV9281_REG8(0x4507), 0x00},
	{OV9281_REG8(0x4509), 0x00}, {OV9281_REG8(0x450a), 0x08}, {OV9281_REG8(0x4601), 0x30},
	{OV9281_REG8(0x470f), 0x00}, {OV9281_REG8(0x4f07), 0x00}, {OV9281_REG8(0x4800), 0x60},
	{OV9281_REG8(0x4837), 0x14}, {OV9281_REG8(0x5000), 0x9f}, {OV9281_REG8(0x5001), 0x00},
	{OV9281_REG8(0x5e00), 0x00}, {OV9281_REG8(0x5d00), 0x07}, {OV9281_REG8(0x5d01), 0x00},
	{OV9281_REG8(0x4f00), 0x0c}, {OV9281_REG8(0x4f10), 0x00}, {OV9281_REG8(0x4f11), 0x88},
	{OV9281_REG8(0x4f12), 0x0f}, {OV9281_REG8(0x4f13), 0xc4},
};

/*
 * Alp-authored, NOT from Espressif -- BENCH-VERIFIED 2026-09-21 on an E1M-AEN803 on
 * the E1M-EVK: live frames captured in this mode and the VTS projection below matches
 * the measured wall-clock frame rate. Espressif's ov9281 upstream ships no 1280x800
 * (full-array) mode; this table is derived from
 * ov9281_mode_1280x720_50fps_regs above (same PLL config 0x0302/0x030d/0x030e, same HTS
 * 0x380c/0x380d = 0x0369, no column/row skip 0x3814/0x3815 = 0x11/0x11, no mirror/flip
 * 0x3820/0x3821 = 0x40/0x00, same X window 0x3800..0x3805, same ISP offsets
 * 0x3810..0x3813 = 0x08/0x08) by changing ONLY:
 *   - 0x3803 (Y start):      0x28 -> 0x00           (row 0, was row 40)
 *   - 0x3806/0x3807 (Y end): 0x0307 -> 0x032f        (row 815 = the full 816-row array;
 *                                                      the same Y-end window Espressif's own
 *                                                      640x400 table already uses)
 *   - 0x380a/0x380b (out height): 0x02d0 -> 0x0320   (800)
 *   - 0x380e/0x380f (VTS):        0x071c -> 0x038e   (910 lines)
 * VTS 910 matches the 1280x720 mode's line rate: VTS x fps = 91000 lines/s is unchanged
 * at the same HTS and PLL, so HTS x VTS x fps is unchanged (HTS 873 x VTS 1820 x 50 fps ==
 * HTS 873 x VTS 910 x 100 fps == 91000 lines/s). Confirmed by the 2026-09-21 bench
 * measurement: a 60-frame burst completed in 594 ms, ~100 fps.
 */
static const struct video_reg ov9281_mode_1280x800_100fps_regs[] = {
	{OV9281_REG8(0x0103), 0x01}, {OV9281_REG8(0x0106), 0x00}, {OV9281_REG8(0x0302), 0x32},
	{OV9281_REG8(0x030d), 0x50}, {OV9281_REG8(0x030e), 0x02}, {OV9281_REG8(0x3001), 0x00},
	{OV9281_REG8(0x3004), 0x00}, {OV9281_REG8(0x3005), 0x00}, {OV9281_REG8(0x3006), 0x04},
	{OV9281_REG8(0x3011), 0x0a}, {OV9281_REG8(0x3013), 0x18}, {OV9281_REG8(0x301c), 0xf0},
	{OV9281_REG8(0x3022), 0x01}, {OV9281_REG8(0x3030), 0x10}, {OV9281_REG8(0x3039), 0x32},
	{OV9281_REG8(0x303a), 0x00}, {OV9281_REG8(0x3500), 0x00}, {OV9281_REG8(0x3501), 0x2a},
	{OV9281_REG8(0x3502), 0x90}, {OV9281_REG8(0x3503), 0x08}, {OV9281_REG8(0x3505), 0x8c},
	{OV9281_REG8(0x3507), 0x03}, {OV9281_REG8(0x3508), 0x00},
	{OV9281_REG8(0x3509), OV9281_GAIN_DEFAULT},
	{OV9281_REG8(0x3610), 0x80}, {OV9281_REG8(0x3611), 0xa0}, {OV9281_REG8(0x3620), 0x6e},
	{OV9281_REG8(0x3632), 0x56}, {OV9281_REG8(0x3633), 0x78}, {OV9281_REG8(0x3662), 0x07},
	{OV9281_REG8(0x3666), 0x00}, {OV9281_REG8(0x366f), 0x5a}, {OV9281_REG8(0x3680), 0x84},
	{OV9281_REG8(0x3707), 0x56}, {OV9281_REG8(0x370d), 0x00}, {OV9281_REG8(0x370e), 0xfa},
	{OV9281_REG8(0x3712), 0x80}, {OV9281_REG8(0x372d), 0x22}, {OV9281_REG8(0x3731), 0x80},
	{OV9281_REG8(0x3732), 0x30}, {OV9281_REG8(0x3778), 0x00}, {OV9281_REG8(0x377d), 0x22},
	{OV9281_REG8(0x3788), 0x02}, {OV9281_REG8(0x3789), 0xa4}, {OV9281_REG8(0x378a), 0x00},
	{OV9281_REG8(0x378b), 0x4a}, {OV9281_REG8(0x3799), 0x20}, {OV9281_REG8(0x379c), 0x01},
	{OV9281_REG8(0x3800), 0x00}, {OV9281_REG8(0x3801), 0x00}, {OV9281_REG8(0x3802), 0x00},
	{OV9281_REG8(0x3803), 0x00}, {OV9281_REG8(0x3804), 0x05}, {OV9281_REG8(0x3805), 0x0f},
	{OV9281_REG8(0x3806), 0x03}, {OV9281_REG8(0x3807), 0x2f}, {OV9281_REG8(0x3808), 0x05},
	{OV9281_REG8(0x3809), 0x00}, {OV9281_REG8(0x380a), 0x03}, {OV9281_REG8(0x380b), 0x20},
	{OV9281_REG8(0x380c), 0x03}, {OV9281_REG8(0x380d), 0x69}, {OV9281_REG8(0x380e), 0x03},
	{OV9281_REG8(0x380f), 0x8e}, {OV9281_REG8(0x3810), 0x00}, {OV9281_REG8(0x3811), 0x08},
	{OV9281_REG8(0x3812), 0x00}, {OV9281_REG8(0x3813), 0x08}, {OV9281_REG8(0x3814), 0x11},
	{OV9281_REG8(0x3815), 0x11}, {OV9281_REG8(0x3820), 0x40}, {OV9281_REG8(0x3821), 0x00},
	{OV9281_REG8(0x382b), 0x3a}, {OV9281_REG8(0x382c), 0x06}, {OV9281_REG8(0x382d), 0xc2},
	{OV9281_REG8(0x389d), 0x00}, {OV9281_REG8(0x3881), 0x42}, {OV9281_REG8(0x3882), 0x02},
	{OV9281_REG8(0x3883), 0x12}, {OV9281_REG8(0x3885), 0x07}, {OV9281_REG8(0x38a8), 0x02},
	{OV9281_REG8(0x38a9), 0x80}, {OV9281_REG8(0x38b1), 0x03}, {OV9281_REG8(0x38b3), 0x07},
	{OV9281_REG8(0x38c4), 0x00}, {OV9281_REG8(0x38c5), 0xc0}, {OV9281_REG8(0x38c6), 0x04},
	{OV9281_REG8(0x38c7), 0x80}, {OV9281_REG8(0x3920), 0xff},
	{OV9281_REG8(0x4003), OV9281_BLC_TARGET_DEFAULT},
	{OV9281_REG8(0x4008), 0x04}, {OV9281_REG8(0x4009), 0x0b}, {OV9281_REG8(0x400c), 0x01},
	{OV9281_REG8(0x400d), 0x07}, {OV9281_REG8(0x4010), 0xf0}, {OV9281_REG8(0x4011), 0x3b},
	{OV9281_REG8(0x4042), 0x01}, {OV9281_REG8(0x4043), 0x40}, {OV9281_REG8(0x4307), 0x30},
	{OV9281_REG8(0x4317), 0x00}, {OV9281_REG8(0x4501), 0x00}, {OV9281_REG8(0x4507), 0x00},
	{OV9281_REG8(0x4509), 0x00}, {OV9281_REG8(0x450a), 0x08}, {OV9281_REG8(0x4601), 0x30},
	{OV9281_REG8(0x470f), 0x00}, {OV9281_REG8(0x4f07), 0x00}, {OV9281_REG8(0x4800), 0x60},
	{OV9281_REG8(0x4837), 0x14}, {OV9281_REG8(0x5000), 0x9f}, {OV9281_REG8(0x5001), 0x00},
	{OV9281_REG8(0x5e00), 0x00}, {OV9281_REG8(0x5d00), 0x07}, {OV9281_REG8(0x5d01), 0x00},
	{OV9281_REG8(0x4f00), 0x0c}, {OV9281_REG8(0x4f10), 0x00}, {OV9281_REG8(0x4f11), 0x88},
	{OV9281_REG8(0x4f12), 0x0f}, {OV9281_REG8(0x4f13), 0xc4},
};

/* Espressif ov9281_isp_info[] pclk/vts/hts, cross-checked as hts*vts*fps ~= pclk */
enum ov9281_mode_id {
	OV9281_MODE_640X400_100FPS,
	OV9281_MODE_1280X720_50FPS,
	OV9281_MODE_1280X800_100FPS,
};

struct ov9281_mode {
	const struct video_reg *regs;
	size_t regs_len;
	uint32_t width;
	uint32_t height;
	uint32_t framerate;
	uint32_t vts;
	uint32_t pixel_rate;
};

static const struct ov9281_mode ov9281_modes[] = {
	[OV9281_MODE_640X400_100FPS] = {
		.regs = ov9281_mode_640x400_100fps_regs,
		.regs_len = ARRAY_SIZE(ov9281_mode_640x400_100fps_regs),
		.width = 640, .height = 400, .framerate = 100,
		.vts = 1096, .pixel_rate = 160000000,
	},
	[OV9281_MODE_1280X720_50FPS] = {
		.regs = ov9281_mode_1280x720_50fps_regs,
		.regs_len = ARRAY_SIZE(ov9281_mode_1280x720_50fps_regs),
		.width = 1280, .height = 720, .framerate = 50,
		.vts = 1820, .pixel_rate = 158886158,
	},
	/* Same PLL/HTS as the 1280x720 mode above, so the same pixel_rate: VTS x fps =
	 * 91000 lines/s is unchanged at the same HTS and PLL, so HTS x VTS x fps is unchanged
	 * (1820*50 == 910*100 == 91000 lines/s), so 158886158 carries over unchanged rather
	 * than being recomputed. */
	[OV9281_MODE_1280X800_100FPS] = {
		.regs = ov9281_mode_1280x800_100fps_regs,
		.regs_len = ARRAY_SIZE(ov9281_mode_1280x800_100fps_regs),
		.width = 1280, .height = 800, .framerate = 100,
		.vts = 910, .pixel_rate = 158886158,
	},
};

static const struct video_format_cap ov9281_fmts[] = {
	[OV9281_MODE_640X400_100FPS] = {
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width_min = 640, .width_max = 640, .width_step = 1,
		.height_min = 400, .height_max = 400, .height_step = 1,
	},
	[OV9281_MODE_1280X720_50FPS] = {
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width_min = 1280, .width_max = 1280, .width_step = 1,
		.height_min = 720, .height_max = 720, .height_step = 1,
	},
	[OV9281_MODE_1280X800_100FPS] = {
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width_min = 1280, .width_max = 1280, .width_step = 1,
		.height_min = 800, .height_max = 800, .height_step = 1,
	},
	{0},
};

static const int64_t ov9281_link_frequencies[] = {
	OV9281_LINK_FREQ_HZ,
};

struct ov9281_ctrls {
	struct video_ctrl exposure;
	struct video_ctrl analogue_gain;
	struct video_ctrl test_pattern;
	struct video_ctrl pixel_rate;
	struct video_ctrl link_freq;
};

struct ov9281_data {
	struct ov9281_ctrls ctrls;
	struct video_format fmt;
	enum ov9281_mode_id mode;
};

struct ov9281_config {
	struct i2c_dt_spec i2c;
};

/* Forward declaration: ov9281_set_fmt() re-applies the video_ctrl cache through this after a
 * real mode reload, ahead of its own definition further down this file. */
static int ov9281_set_ctrl(const struct device *dev, uint32_t cid);

static int ov9281_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct ov9281_config *cfg = dev->config;
	struct ov9281_data *data = dev->data;
	const struct ov9281_mode *mode;
	size_t idx;
	int ret;

	ret = video_format_caps_index(ov9281_fmts, fmt, &idx);
	if (ret < 0) {
		LOG_ERR("Format '%s' %ux%u not supported", VIDEO_FOURCC_TO_STR(fmt->pixelformat),
			fmt->width, fmt->height);
		return -ENOTSUP;
	}

	/*
	 * zephyr_video.c's get_fmt path round-trips through video_get_format() -> set_format(),
	 * so a real caller (the CPI backend querying the negotiated format) hits this function
	 * far more often than a genuine mode change. Every entry in ov9281_mode_*_regs[] starts
	 * with a 0x0103 soft reset (Espressif's table, kept verbatim), so reloading it
	 * unconditionally would stop an active stream and revert exposure/gain/test-pattern to
	 * Espressif's mode defaults on every no-op query -- while the video_ctrl cache below
	 * keeps reporting whatever the user last set via VIDEO_CID_*, silently diverging from the
	 * sensor. Skip the reload entirely when the requested mode is already the active one.
	 */
	if (idx == data->mode && data->fmt.pixelformat == fmt->pixelformat) {
		return 0;
	}

	mode = &ov9281_modes[idx];

	ret = video_write_cci_multiregs(&cfg->i2c, mode->regs, mode->regs_len);
	if (ret < 0) {
		return ret;
	}

	data->fmt = *fmt;
	data->mode = idx;

	/* The exposure ceiling and the pixel rate are per-mode (VTS/pixel-clock differ) */
	data->ctrls.exposure.range.max = mode->vts - OV9281_EXP_MAX_OFFSET;
	if (data->ctrls.exposure.val > data->ctrls.exposure.range.max) {
		data->ctrls.exposure.val = data->ctrls.exposure.range.max;
	}
	data->ctrls.pixel_rate.range.min64 = mode->pixel_rate;
	data->ctrls.pixel_rate.range.max64 = mode->pixel_rate;
	data->ctrls.pixel_rate.val64 = mode->pixel_rate;

	/*
	 * A real reload just soft-reset the sensor to Espressif's per-mode register defaults,
	 * which for VIDEO_CID_EXPOSURE/ANALOGUE_GAIN/TEST_PATTERN overwrites whatever the caller
	 * had set through set_ctrl() -- the video_ctrl cache itself is untouched by the reset, so
	 * re-push it now. This runs at driver init too (ov9281_init()'s first, unconditional
	 * mode load), which is why ov9281_init() now calls ov9281_init_ctrls() *before*
	 * ov9281_set_fmt() -- ctrls->*.val must already hold a real default by this point, not a
	 * zero-initialized one.
	 */
	ret = ov9281_set_ctrl(dev, VIDEO_CID_EXPOSURE);
	if (ret < 0) {
		return ret;
	}

	ret = ov9281_set_ctrl(dev, VIDEO_CID_ANALOGUE_GAIN);
	if (ret < 0) {
		return ret;
	}

	return ov9281_set_ctrl(dev, VIDEO_CID_TEST_PATTERN);
}

static int ov9281_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct ov9281_data *data = dev->data;

	*fmt = data->fmt;

	return 0;
}

static int ov9281_get_caps(const struct device *dev, struct video_caps *caps)
{
	if (caps->type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	caps->format_caps = ov9281_fmts;

	return 0;
}

static int ov9281_enum_frmival(const struct device *dev, struct video_frmival_enum *fie)
{
	size_t idx;
	int ret;

	ret = video_format_caps_index(ov9281_fmts, fie->format, &idx);
	if (ret < 0 || fie->index != 0) {
		return -EINVAL;
	}

	fie->type = VIDEO_FRMIVAL_TYPE_DISCRETE;
	fie->discrete.numerator = 1;
	fie->discrete.denominator = ov9281_modes[idx].framerate;

	return 0;
}

static int ov9281_set_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct ov9281_data *data = dev->data;

	/* Each mode has exactly one fixed frame rate -- fixed up to whatever the mode gives */
	frmival->numerator = 1;
	frmival->denominator = ov9281_modes[data->mode].framerate;

	return 0;
}

static int ov9281_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct ov9281_data *data = dev->data;

	frmival->numerator = 1;
	frmival->denominator = ov9281_modes[data->mode].framerate;

	return 0;
}

static int ov9281_set_stream(const struct device *dev, bool on, enum video_buf_type type)
{
	const struct ov9281_config *cfg = dev->config;

	if (type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	return video_write_cci_reg(&cfg->i2c, OV9281_REG_CTRL_MODE,
				   on ? OV9281_MODE_STREAMING : OV9281_MODE_SW_STANDBY);
}

static int ov9281_set_ctrl(const struct device *dev, uint32_t cid)
{
	const struct ov9281_config *cfg = dev->config;
	struct ov9281_data *data = dev->data;
	struct ov9281_ctrls *ctrls = &data->ctrls;
	uint32_t exposure;
	int ret;

	switch (cid) {
	case VIDEO_CID_EXPOSURE:
		exposure = CLAMP(ctrls->exposure.val, OV9281_EXPOSURE_MIN,
				 ctrls->exposure.range.max);
		ret = video_write_cci_reg(&cfg->i2c, OV9281_REG_EXPOSURE_H,
					  OV9281_FETCH_EXP_H(exposure));
		if (ret < 0) {
			return ret;
		}
		ret = video_write_cci_reg(&cfg->i2c, OV9281_REG_EXPOSURE_M,
					  OV9281_FETCH_EXP_M(exposure));
		if (ret < 0) {
			return ret;
		}
		return video_write_cci_reg(&cfg->i2c, OV9281_REG_EXPOSURE_L,
					   OV9281_FETCH_EXP_L(exposure));
	case VIDEO_CID_ANALOGUE_GAIN:
		return video_write_cci_reg(&cfg->i2c, OV9281_REG_GAIN,
					   ov9281_gain_reg_map[ctrls->analogue_gain.val]);
	case VIDEO_CID_TEST_PATTERN:
		return video_modify_cci_reg(&cfg->i2c, OV9281_REG_TEST_PATTERN,
					    OV9281_TEST_PATTERN_ENABLE,
					    ctrls->test_pattern.val != 0
						    ? OV9281_TEST_PATTERN_ENABLE
						    : 0);
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(video, ov9281_driver_api) = {
	.set_format = ov9281_set_fmt,
	.get_format = ov9281_get_fmt,
	.get_caps = ov9281_get_caps,
	.set_stream = ov9281_set_stream,
	.set_ctrl = ov9281_set_ctrl,
	.set_frmival = ov9281_set_frmival,
	.get_frmival = ov9281_get_frmival,
	.enum_frmival = ov9281_enum_frmival,
};

static const char *const ov9281_test_pattern_menu[] = {
	"Off",
	"On",
	NULL,
};

static int ov9281_init_ctrls(const struct device *dev)
{
	struct ov9281_data *data = dev->data;
	struct ov9281_ctrls *ctrls = &data->ctrls;
	const struct ov9281_mode *mode = &ov9281_modes[data->mode];
	int ret;

	ret = video_init_ctrl(&ctrls->exposure, dev, VIDEO_CID_EXPOSURE,
			      (struct video_ctrl_range){.min = OV9281_EXPOSURE_MIN,
							.max = mode->vts - OV9281_EXP_MAX_OFFSET,
							.step = 1,
							.def = OV9281_EXPOSURE_DEFAULT});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->analogue_gain, dev, VIDEO_CID_ANALOGUE_GAIN,
			      (struct video_ctrl_range){.min = 0,
							.max = ARRAY_SIZE(ov9281_gain_reg_map) - 1,
							.step = 1,
							.def = OV9281_GAIN_DEFAULT_INDEX});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_menu_ctrl(&ctrls->test_pattern, dev, VIDEO_CID_TEST_PATTERN, 0,
				   ov9281_test_pattern_menu);
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->pixel_rate, dev, VIDEO_CID_PIXEL_RATE,
			      (struct video_ctrl_range){.min64 = mode->pixel_rate,
							.max64 = mode->pixel_rate,
							.step64 = 1,
							.def64 = mode->pixel_rate});
	if (ret < 0) {
		return ret;
	}
	ctrls->pixel_rate.flags |= VIDEO_CTRL_FLAG_READ_ONLY;

	ret = video_init_int_menu_ctrl(&ctrls->link_freq, dev, VIDEO_CID_LINK_FREQ, 0,
				       ov9281_link_frequencies,
				       ARRAY_SIZE(ov9281_link_frequencies));
	if (ret < 0) {
		return ret;
	}
	ctrls->link_freq.flags |= VIDEO_CTRL_FLAG_READ_ONLY;

	return 0;
}

static int ov9281_init(const struct device *dev)
{
	const struct ov9281_config *cfg = dev->config;
	struct ov9281_data *data = dev->data;
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width = ov9281_modes[OV9281_MODE_1280X720_50FPS].width,
		.height = ov9281_modes[OV9281_MODE_1280X720_50FPS].height,
	};
	uint32_t chip_id;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	ret = video_write_cci_reg(&cfg->i2c, OV9281_REG_SW_RESET, 0x01);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(5));

	ret = video_read_cci_reg(&cfg->i2c, OV9281_REG_CHIP_ID, &chip_id);
	if (ret < 0) {
		return ret;
	}

	if (chip_id != OV9281_CHIP_ID) {
		LOG_ERR("Wrong chip ID 0x%04x instead of 0x%04x", chip_id, OV9281_CHIP_ID);
		return -ENODEV;
	}

	data->mode = OV9281_MODE_1280X720_50FPS;

	/*
	 * Controls before the mode load: ov9281_set_fmt() re-applies the video_ctrl cache
	 * (exposure/gain/test-pattern) after every REAL mode reload -- including this first one
	 * -- so ctrls->*.val must already hold a real default, not a zero-initialized one, by the
	 * time set_fmt() runs.
	 */
	ret = ov9281_init_ctrls(dev);
	if (ret < 0) {
		return ret;
	}

	return ov9281_set_fmt(dev, &fmt);
}

#define OV9281_EP(n) DT_CHILD(DT_INST_CHILD(n, port), endpoint)
#define OV9281_INPUT_CLK(n) DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency)

#define OV9281_INIT(n)                                                                            \
	BUILD_ASSERT(DT_PROP_OR(OV9281_EP(n), bus_type, VIDEO_BUS_TYPE_CSI2_DPHY) ==              \
			     VIDEO_BUS_TYPE_CSI2_DPHY,                                            \
		     "Only the MIPI CSI-2 D-PHY interface is supported");                        \
	BUILD_ASSERT(DT_PROP_LEN_OR(OV9281_EP(n), data_lanes, 2) == 2,                           \
		     "Only the two data lanes mode is supported");                               \
	BUILD_ASSERT(OV9281_INPUT_CLK(n) == OV9281_INPUT_CLK_HZ,                                 \
		     "XVCLK must be 24 MHz -- the only input clock Espressif's mode tables "     \
		     "were generated for");                                                      \
                                                                                                   \
	static struct ov9281_data ov9281_data_##n;                                               \
                                                                                                   \
	static const struct ov9281_config ov9281_cfg_##n = {                                     \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &ov9281_init, NULL, &ov9281_data_##n, &ov9281_cfg_##n,           \
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &ov9281_driver_api);       \
                                                                                                   \
	VIDEO_DEVICE_DEFINE(ov9281_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(OV9281_INIT)
