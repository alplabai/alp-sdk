/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR-0017-ADJACENT, BENCH-UNVERIFIED ======
 * Sony IMX296 1.58 MP global-shutter MIPI CSI-2 image sensor, authored from
 * the Sony IMX296 datasheet (register map + timing sections). No
 * permissive-licence Zephyr/vendor driver exists to consume for this part:
 * upstream Zephyr v4.4.1 ships no "sony,imx296" driver, hal_alif carries no
 * IMX296 register lib, and Espressif esp_cam_sensor has no IMX296 port (it
 * has ov9281, consumed separately in ov9281.c). The only other IMX296
 * drivers known to exist are Linux's (GPL-2.0, not consumable) and
 * libcamera's helpers (also not consumable) -- neither was opened, fetched
 * or read while writing this file; every register address, value and
 * per-INCK table below is cited to a datasheet section/table in the comment
 * above it, not copied from any other driver.
 *
 * BENCH-UNVERIFIED: not yet run against real IMX296 silicon (no
 * raspberry_pi_global_shutter_camera shield bench pass on this batch).
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
 * Datasheet "Setting Registers Using Serial Communication" -> "Description
 * of Setting Registers (I2C)": the sensor answers on one of three 7-bit CCI
 * addresses depending on the SLAMODE pin strap (0x36 / 0x37), or 0x1A
 * regardless of SLAMODE polarity ("SLAVE Address (SLAMODE = 0 / 1)"). 0x1A
 * is used here since the module's SLAMODE strap is not under this driver's
 * control and 0x1A works either way -- see zephyr/dts/bindings/video/
 * sony,imx296.yaml.
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
 * digital gain, 0.1 dB step, 0 (0 dB) to 480 (48.0 dB).
 */
#define IMX296_REG_GAIN IMX296_REG16(0x3204)
#define IMX296_GAIN_MAX 480

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
 * or more)". This driver only waits out the mandatory 1 ms regulator
 * settling time here; the remaining per-frame ramp-up is a capture-quality
 * concern left to the consumer, not a register-timing requirement.
 */
#define IMX296_STANDBY_SETTLE_MS 1

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
};

struct imx296_data {
	struct imx296_ctrls ctrls;
	struct video_format fmt;
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
	const struct imx296_config *cfg = dev->config;
	int                         ret;

	if (type != VIDEO_BUF_TYPE_OUTPUT) {
		LOG_ERR("Only output buffers supported");
		return -EINVAL;
	}

	if (on) {
		/* "Standby mode" (page 54): cancel standby, then wait for regulator settling */
		ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, 0);
		if (ret < 0) {
			return ret;
		}

		k_sleep(K_MSEC(IMX296_STANDBY_SETTLE_MS));

		/* "Slave Mode and Master Mode" (page 55): start master-mode free-run */
		return video_write_cci_reg(&cfg->i2c, IMX296_REG_XMSTA, 0);
	}

	ret = video_write_cci_reg(&cfg->i2c, IMX296_REG_XMSTA, IMX296_XMSTA_STOP);
	if (ret < 0) {
		return ret;
	}

	return video_write_cci_reg(&cfg->i2c, IMX296_REG_STANDBY, IMX296_STANDBY_STANDBY);
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

	ret = video_init_ctrl(&ctrls->exposure,
	                      dev,
	                      VIDEO_CID_EXPOSURE,
	                      (struct video_ctrl_range){ .min  = 1,
	                                                 .max  = IMX296_VMAX - IMX296_SHS_MIN,
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
	int      ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	/*
	 * The datasheet's register map defines no readable chip/product-ID register for this
	 * part (its "Chip ID = 02h".."13h" headings name register-address BANKS -- the upper
	 * byte of the 16-bit CCI address -- not a device-identification value). As a documented
	 * readable-register connectivity sanity check instead, read back STANDBY (page 34) and
	 * require only an ACKed CCI transaction, not a specific bit value: the RPi-style J5
	 * connector this part sits behind carries no reset line, and the module stays powered
	 * across a SoC warm reset/reflash, so STANDBY can legitimately read back 0 (already
	 * running) on any init after the first -- rejecting that as -ENODEV would silently skip
	 * every register write and control registration below, leaving the CSI-2 link with no
	 * sensor behind it. video_read_cci_reg() already returns a negative errno on I2C NAK/
	 * timeout, so a successful return here IS the connectivity proof.
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
