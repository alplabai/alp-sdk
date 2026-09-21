/*
 * Copyright (C) 2026 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM) ======
 * The Alif Ensemble CPI parallel-camera capture controller is driven by a
 * vendored copy of the Apache-2.0 zephyr_alif fork driver
 * (drivers/video/video_alif.c, compatible "alif,cam").  Upstream Zephyr v4.4 +
 * hal_alif ship NO camera-capture (CPI) class driver, so this is a genuine
 * fork-driver copy carried in-tree so it survives a `west update`.  Retire onto
 * the opt-in sdk-alif fork compatible once the cam nodes are repointed AND
 * bench-verified.  See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 *
 * Vendored from the fork, then PORTED to the upstream Zephyr v4.4 video API by
 * Alp Lab AB.  The fork driver targeted the OLDER video API -- its
 * video_driver_api callbacks took `enum video_endpoint_id ep`
 * (set_format/get_format/get_caps/flush/enqueue/dequeue/set_signal) and a
 * value-pointer ctrl API, which upstream v4.4 REMOVED.  The v4.4 deltas applied
 * here (each marked "v4.4 video-API shim (Alp Lab AB)" at the call site):
 *   - dropped the `enum video_endpoint_id ep` param + its VIDEO_EP_OUT/ALL
 *     validation from set_format/get_format/get_caps/flush/enqueue/dequeue/
 *     set_signal; the forwarding helpers (video_set_format/video_get_format/
 *     video_get_caps) lose their `ep` arg;
 *   - set_stream(dev, bool) gained an `enum video_buf_type type` param;
 *     video_stream_start/_stop now take VIDEO_BUF_TYPE_OUTPUT;
 *   - the value-pointer forwarding .set_ctrl/.get_ctrl callbacks are dropped
 *     (the CPI owns no controls -- v4.4 controls are registry-owned and
 *     addressed directly on the endpoint device);
 *   - legacy Bayer/greyscale pixfmt names (BGGR8.. / Y6P / Y7P) are aliased in
 *     the vendored <zephyr/drivers/video/video_alif.h>.
 * The driver now COMPILES against v4.4 (the ALP_VIDEO_ALIF_BROKEN gate is
 * retired).  vendor-ext, BENCH-UNVERIFIED: the J5 camera shields are now
 * described and build-tested, but no live frame capture has completed on this
 * path.
 *
 * Alp Lab AB: the fork's alif_cam_work_helper() stopped the endpoint on
 * IN-FIFO starvation and never restarted it -- the next enqueue() only ever
 * called k_fifo_put(), so a consumer that held its one buffer for more than a
 * frame period killed the stream for good (bench-proven 2026-09-21 on an
 * E1M-AEN803 on the E1M-EVK, OV9281).  Added `starved` + a `lock` (struct
 * video_cam_data, video_alif.h) so starvation now PAUSES capture and
 * enqueue() RESUMES it; see the "Buffer-starvation/resume contract" comments
 * at the work helper and at enqueue().
 *
 * The soc_memory_map.h include resolves via the hal_alif common/include dir
 * (gated on CONFIG_RTSS_HE/HP); the public CSI data-type + CPI-mode tables come
 * from <zephyr/drivers/video/video_alif.h> (also vendored here).
 */
#define DT_DRV_COMPAT alif_cam

#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/drivers/pinctrl.h>

#include "video_alif.h"
#include <zephyr/drivers/video/video_alif.h>
#include <soc_memory_map.h>
#include <zephyr/cache.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(CPI, CONFIG_VIDEO_LOG_LEVEL);

/* Alp Lab AB: on the E8, CAM_CFG.AXI_PORT_EN (gated by CONFIG_VIDEO_ALIF_CAM_EXTENDED,
 * see alif_video_cam_set_config()) is what makes the CPI's AXI master actually write a
 * captured frame to memory -- without it the CPI still raises STOP for every frame, so a
 * capture silently "succeeds" over an untouched buffer instead of failing (the bug this
 * driver's #226 fix chased). Catch a build that disables the option on an E8 target before
 * it ships a camera path that never writes memory.
 *
 * Gated on CONFIG_VIDEO_ALIF_CAM too: this TU also builds standalone (see
 * zephyr/CMakeLists.txt) to supply fourcc_to_plane_size()/fourcc_to_numplanes()/
 * pix_fmt_bpp() to CONFIG_VIDEO_ISP_VSI when no "alif,cam" DT node is enabled at
 * all (e.g. aen-isp-regcheck) -- DT_INST_FOREACH_STATUS_OKAY(CPI_DEFINE) then
 * expands to nothing and CONFIG_VIDEO_ALIF_CAM_EXTENDED (which `depends on
 * VIDEO_ALIF_CAM`) cannot be y regardless of the E8 default, with no CPI
 * instance for the untouched-buffer bug to apply to.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_VIDEO_ALIF_CAM) || !IS_ENABLED(CONFIG_SOC_SERIES_E8) ||
		     IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED),
	     "CONFIG_VIDEO_ALIF_CAM_EXTENDED must stay on for the E8: without it the CPI "
	     "never writes a captured frame to memory (CAM_CFG.AXI_PORT_EN stays clear), "
	     "so alp_camera_capture() reports success over an untouched buffer");

#define WORKQ_STACK_SIZE 512
#define WORKQ_PRIORITY   7
K_KERNEL_STACK_DEFINE(alif_isr_cb_workq, WORKQ_STACK_SIZE);

extern size_t fourcc_to_plane_size(uint32_t fourcc, uint8_t plane_id, size_t buffer_size);
extern int fourcc_to_numplanes(uint32_t fourcc);
extern unsigned int pix_fmt_bpp(uint32_t fmt);

size_t fourcc_to_plane_size(uint32_t fourcc, uint8_t plane_id, size_t buffer_size)
{
	switch (fourcc)	{
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
	case VIDEO_PIX_FMT_BGGR10:
	case VIDEO_PIX_FMT_GBRG10:
	case VIDEO_PIX_FMT_GRBG10:
	case VIDEO_PIX_FMT_RGGB10:
	case VIDEO_PIX_FMT_BGGR12:
	case VIDEO_PIX_FMT_GBRG12:
	case VIDEO_PIX_FMT_GRBG12:
	case VIDEO_PIX_FMT_RGGB12:
	case VIDEO_PIX_FMT_BGGR14:
	case VIDEO_PIX_FMT_GBRG14:
	case VIDEO_PIX_FMT_GRBG14:
	case VIDEO_PIX_FMT_RGGB14:
	case VIDEO_PIX_FMT_BGGR16:
	case VIDEO_PIX_FMT_GBRG16:
	case VIDEO_PIX_FMT_GRBG16:
	case VIDEO_PIX_FMT_RGGB16:
	case VIDEO_PIX_FMT_GREY:
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_Y12P:
	case VIDEO_PIX_FMT_YVYU:
	case VIDEO_PIX_FMT_YUYV:
	case VIDEO_PIX_FMT_VYUY:
	case VIDEO_PIX_FMT_UYVY:
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_RGB24:
	case VIDEO_PIX_FMT_BGR24:
		if (plane_id == 0) {
			return buffer_size;
		} else {
			return 0;
		}
	case VIDEO_PIX_FMT_NV12:
	case VIDEO_PIX_FMT_NV21:
		if (plane_id == 0) {
			return (buffer_size << 1) / 3;
		} else if (plane_id == 1) {
			return (buffer_size / 3);
		} else {
			return 0;
		}
		break;
	case VIDEO_PIX_FMT_NV16:
	case VIDEO_PIX_FMT_NV61:
		if (plane_id == 0 || plane_id == 1) {
			return (buffer_size >> 1);
		} else {
			return 0;
		}
	case VIDEO_PIX_FMT_NV24:
	case VIDEO_PIX_FMT_NV42:
		if (plane_id == 0) {
			return (buffer_size / 3);
		} else if (plane_id == 1) {
			return ((buffer_size << 1) / 3);
		} else {
			return 0;
		}
	case VIDEO_PIX_FMT_YUV422P:
		if (plane_id == 0) {
			return (buffer_size >> 1);
		} else if (plane_id == 1 || plane_id == 2) {
			return (buffer_size >> 2);
		} else {
			return 0;
		}
	case VIDEO_PIX_FMT_YUV420:
		if (plane_id == 0) {
			return (buffer_size << 1) / 3;
		} else if (plane_id == 1 || plane_id == 2) {
			return (buffer_size / 6);
		} else {
			return 0;
		}
	case VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE:
		if (plane_id == 0 || plane_id == 1 || plane_id == 2) {
			return (buffer_size / 3);
		} else {
			return 0;
		}
	default:
		return UINT_MAX;
	}
}

int fourcc_to_numplanes(uint32_t fourcc)
{
	switch (fourcc) {
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
	case VIDEO_PIX_FMT_BGGR10:
	case VIDEO_PIX_FMT_GBRG10:
	case VIDEO_PIX_FMT_GRBG10:
	case VIDEO_PIX_FMT_RGGB10:
	case VIDEO_PIX_FMT_BGGR12:
	case VIDEO_PIX_FMT_GBRG12:
	case VIDEO_PIX_FMT_GRBG12:
	case VIDEO_PIX_FMT_RGGB12:
	case VIDEO_PIX_FMT_BGGR14:
	case VIDEO_PIX_FMT_GBRG14:
	case VIDEO_PIX_FMT_GRBG14:
	case VIDEO_PIX_FMT_RGGB14:
	case VIDEO_PIX_FMT_BGGR16:
	case VIDEO_PIX_FMT_GBRG16:
	case VIDEO_PIX_FMT_GRBG16:
	case VIDEO_PIX_FMT_RGGB16:
	case VIDEO_PIX_FMT_GREY:
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_Y12P:
	case VIDEO_PIX_FMT_YUYV:
	case VIDEO_PIX_FMT_YVYU:
	case VIDEO_PIX_FMT_VYUY:
	case VIDEO_PIX_FMT_UYVY:
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_RGB24:
	case VIDEO_PIX_FMT_BGR24:
		return 1;
	case VIDEO_PIX_FMT_NV12:
	case VIDEO_PIX_FMT_NV21:
	case VIDEO_PIX_FMT_NV16:
	case VIDEO_PIX_FMT_NV61:
	case VIDEO_PIX_FMT_NV24:
	case VIDEO_PIX_FMT_NV42:
		return 2;
	case VIDEO_PIX_FMT_YUV422P:
	case VIDEO_PIX_FMT_YUV420:
	case VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE:
		return 3;
	default:
		return 0;
	}
}

static void reg_write_part(uintptr_t reg, uint32_t data, uint32_t mask, uint8_t shift)
{
	uint32_t tmp = 0;

	tmp = sys_read32(reg);
	tmp &= ~(mask << shift);
	tmp |= (data & mask) << shift;
	sys_write32(tmp, reg);
}

unsigned int pix_fmt_bpp(uint32_t fmt)
{
	uint32_t ret;

	ret = video_bits_per_pixel(fmt);
	if (ret) {
		return ret;
	}

	switch (fmt) {
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_YUYV:
		return 16;
	case VIDEO_PIX_FMT_Y6P:
	case VIDEO_PIX_FMT_Y7P:
	case VIDEO_PIX_FMT_GREY:
		return 8;
	case VIDEO_PIX_FMT_Y10P:
		return 10;
	case VIDEO_PIX_FMT_Y12P:
		return 12;
	case VIDEO_PIX_FMT_Y14P:
		return 14;
	case VIDEO_PIX_FMT_Y10:
	case VIDEO_PIX_FMT_Y12:
	case VIDEO_PIX_FMT_Y14:
	case VIDEO_PIX_FMT_Y16:
		return 16;
	default:
		return 0;
	}
}

static inline void hw_enable_interrupts(uintptr_t regs, uint32_t intr_mask)
{
	sys_set_bits(regs + CAM_INTR_ENA, intr_mask);
}

static inline void hw_disable_interrupts(uintptr_t regs, uint32_t intr_mask)
{
	sys_clear_bits(regs + CAM_INTR_ENA, intr_mask);
}

/**
 * @brief Apply CPI soft reset sequence per HWRM Section 17 step 3a-3c
 *
 * Performs the three-step soft reset sequence:
 * a. CAM_CTRL = 0       — prepare for soft reset
 * b. CAM_CTRL = 0x100   — activate soft reset (SW_RESET)
 * c. CAM_CTRL = 0       — stop soft reset
 *
 * @param regs CPI register base address
 */
static inline void hw_cam_soft_reset(uintptr_t regs)
{
	sys_write32(0, regs + CAM_CTRL);
	sys_write32(CAM_CTRL_SW_RESET, regs + CAM_CTRL);
	sys_write32(0, regs + CAM_CTRL);
}

static inline void hw_cam_start_video_capture(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/* Apply soft reset before starting capture */
	hw_cam_soft_reset(regs);

	/* Start video capture. */
	if (config->capture_mode == CPI_CAPTURE_MODE_SNAPSHOT) {
		sys_write32(CAM_CTRL_FIFO_CLK_SEL | CAM_CTRL_SNAPSHOT | CAM_CTRL_START,
			    regs + CAM_CTRL);
	} else {
		sys_write32(CAM_CTRL_FIFO_CLK_SEL | CAM_CTRL_START, regs + CAM_CTRL);
	}
}

static int32_t fourcc_to_csi_data_type(uint32_t fourcc)
{
	/*
	 * RGB formats: data_mode_settings[] (video_alif.h) carries all five
	 * CSI2_DT_RGB* entries, but only the ones below have a matching Zephyr
	 * v4.4.1 fourcc with an IDENTICAL bit layout -- mapped here by bit
	 * width/packing, never by name similarity:
	 *   - CSI2_DT_RGB565 (16 bpp)  <- VIDEO_PIX_FMT_RGB565.
	 *   - CSI2_DT_RGB888 (24 bpp, no pad -- see the table's bits_per_pixel
	 *     field, NOT its CPI_COLOR_MODE_CONFIG_IPI48_XRGB888 name, which is
	 *     the HW's internal 48-bit-IPI-datapath label, not a 32-bit pixel)
	 *     <- VIDEO_PIX_FMT_RGB24 / VIDEO_PIX_FMT_BGR24 (24 bpp, no pad).
	 *     The CSI-2 data-type field encodes bit depth only, not channel
	 *     order -- exactly like CSI2_DT_RAW8 above already covering all
	 *     four BGGR8/GBRG8/GRBG8/RGGB8 Bayer orders under one DT -- so
	 *     both 24 bpp fourccs are accepted here.
	 * Deliberately NOT mapped (no matching Zephyr v4.4.1 fourcc, or a
	 * layout mismatch that would misconfigure the IPI bit width):
	 *   - CSI2_DT_RGB444 (12 bpp), CSI2_DT_RGB555 (15 bpp), CSI2_DT_RGB666
	 *     (18 bpp): Zephyr v4.4.1 <zephyr/drivers/video.h> defines no
	 *     packed fourcc at these widths.
	 *   - VIDEO_PIX_FMT_RGB565X (16 bpp, big-endian byte order): the
	 *     table's RGB565 entry documents little-endian only; nothing here
	 *     confirms the CPI IPI packs big-endian, so mapping it would risk
	 *     a silent byte-swap.
	 *   - VIDEO_PIX_FMT_{A,X}RGB32/{A,X}BGR32/RGBA32/RGBX32/BGRA32/BGRX32
	 *     (32 bpp, alpha or padding byte): CSI2_DT_RGB888 is 24 bpp per
	 *     the table; a 32-bit fourcc would misconfigure the IPI bit width
	 *     and corrupt every captured frame's alignment.
	 * Bench-unverified either way (#226) -- no sensor is wired on this
	 * hardware batch to confirm any of the above against silicon.
	 */
	switch (fourcc) {
	case VIDEO_PIX_FMT_Y6P:
		return CSI2_DT_RAW6;
	case VIDEO_PIX_FMT_Y7P:
		return CSI2_DT_RAW7;
	case VIDEO_PIX_FMT_GREY:
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
		return CSI2_DT_RAW8;
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_SBGGR10P:
	case VIDEO_PIX_FMT_SGBRG10P:
	case VIDEO_PIX_FMT_SGRBG10P:
	case VIDEO_PIX_FMT_SRGGB10P:
		return CSI2_DT_RAW10;
	case VIDEO_PIX_FMT_Y12P:
		return CSI2_DT_RAW12;
	case VIDEO_PIX_FMT_Y14P:
		return CSI2_DT_RAW14;
	case VIDEO_PIX_FMT_Y16:
		return CSI2_DT_RAW16;
	case VIDEO_PIX_FMT_RGB565:
		return CSI2_DT_RGB565;
	case VIDEO_PIX_FMT_RGB24:
	case VIDEO_PIX_FMT_BGR24:
		return CSI2_DT_RGB888;
	}
	return -ENOTSUP;
}

/*
 * Alp Lab AB: MIPI-packed link fourcc -> the unpacked fourcc the CPI really
 * writes to memory.  In CSI mode RAW10/12/14 use CPI_DATA_MODE_16_BIT
 * (data_mode_settings[]): one pixel per LSB-aligned 16-bit half-word, upper
 * bits zero (HWRM 17.1.4.8.4-6) -- upstream's unpacked SBGGR10/Y10/.. layout,
 * NOT the 1.25/1.5/1.75 B/px the packed fourcc describes.  The sensor keeps
 * seeing the packed fourcc; the application sees the unpacked one.
 */
static const struct {
	uint32_t link;
	uint32_t mem;
} cpi_unpacked_fmts[] = {
	{VIDEO_PIX_FMT_SBGGR10P, VIDEO_PIX_FMT_SBGGR10},
	{VIDEO_PIX_FMT_SGBRG10P, VIDEO_PIX_FMT_SGBRG10},
	{VIDEO_PIX_FMT_SGRBG10P, VIDEO_PIX_FMT_SGRBG10},
	{VIDEO_PIX_FMT_SRGGB10P, VIDEO_PIX_FMT_SRGGB10},
	{VIDEO_PIX_FMT_Y10P, VIDEO_PIX_FMT_Y10},
	{VIDEO_PIX_FMT_Y12P, VIDEO_PIX_FMT_Y12},
	{VIDEO_PIX_FMT_Y14P, VIDEO_PIX_FMT_Y14},
};

/*
 * Alp Lab AB: in CSI mode, run the CPI pixel clock (CAMERA_PIXCLK_CTRL) at the
 * rate the CSI bridge just programmed on its own pixel clock.  The HWRM prose
 * names CAMERA_PIXCLK_CTRL as the CPI PIXEL_CLK source in CSI mode, while its
 * block diagram and the fork sample (which uses it only as the sensor XVCLK)
 * point at the CSI PIXCLK; matching the two rates is right under either
 * reading.  No-op when the DT carries neither clock.
 */
static int alif_cam_sync_pixclk(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	uint32_t hz;
	int ret;

	if ((config->pix_cid == NULL) || (config->csi_pix_cid == NULL)) {
		return 0;
	}

	ret = clock_control_get_rate(config->clk_dev, config->csi_pix_cid, &hz);
	if (ret) {
		LOG_ERR("Failed to read the CSI pixel clock! ret - %d", ret);
		return ret;
	}

	ret = clock_control_set_rate(config->clk_dev, config->pix_cid,
				     (clock_control_subsys_rate_t)hz);
	if (ret) {
		LOG_ERR("Failed to set the CPI pixel clock to %u Hz! ret - %d", hz, ret);
		return ret;
	}

	/* set_rate programmed the divisor and CLK_SEL; enable last (DFP order). */
	return clock_control_on(config->clk_dev, config->pix_cid);
}

/* Returns the CPI data mode programmed (>= 0), or a negative errno. */
static int alif_cam_set_csi(const struct device *dev, uint32_t fourcc)
{
	const struct video_cam_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	int32_t tmp;
	int i;

	if (config->is_lpcam) {
		LOG_ERR("LP-CPI controller does not support MIPI-CSI2");
		return -EINVAL;
	}

	/* Set MIPI-CSI as the data source and enable halt function. */
	sys_set_bits(regs + CAM_CFG, CAM_CFG_MIPI_CSI);

	/* Set MIPI-CSI halt-enable function. */
	if (config->csi_halt_en) {
		sys_set_bits(regs + CAM_CFG, CAM_CFG_CSI_HALT_EN);
	}

	tmp = fourcc_to_csi_data_type(fourcc);
	if (tmp < 0) {
		LOG_ERR("Unsupported CSI pixel format.");
		return tmp;
	}

	for (i = 0; i < ARRAY_SIZE(data_mode_settings); i++) {
		if (data_mode_settings[i].dt == tmp) {
			break;
		}
	}

	/* No break taken means i == ARRAY_SIZE, and both uses below then read one
	 * past the end of the table -- writing whatever follows it into
	 * CAM_CSI_CMCFG[MODE] and CAM_CFG[DATA_MODE].  Reachable for any fourcc
	 * mapping to a CSI data type the table lacks (#1825). */
	if (i == ARRAY_SIZE(data_mode_settings)) {
		LOG_ERR("CSI data type 0x%02x has no CPI colour-mode entry", tmp);
		return -ENOTSUP;
	}

	sys_write32(data_mode_settings[i].col_mode, regs + CAM_CSI_CMCFG);

	switch (tmp) {
	case CSI2_DT_RAW10:
		reg_write_part(regs + CAM_CFG, CPI_DATA_MASK_10_BIT, CAM_CFG_DATA_MASK,
			       CAM_CFG_DATA_SHIFT);
		break;
	case CSI2_DT_RAW12:
		reg_write_part(regs + CAM_CFG, CPI_DATA_MASK_12_BIT, CAM_CFG_DATA_MASK,
			       CAM_CFG_DATA_SHIFT);
		break;
	case CSI2_DT_RAW14:
		reg_write_part(regs + CAM_CFG, CPI_DATA_MASK_14_BIT, CAM_CFG_DATA_MASK,
			       CAM_CFG_DATA_SHIFT);
		break;
	case CSI2_DT_RAW16:
	default:
		reg_write_part(regs + CAM_CFG, CPI_DATA_MASK_16_BIT, CAM_CFG_DATA_MASK,
			       CAM_CFG_DATA_SHIFT);
		break;
	}

	reg_write_part(regs + CAM_CFG, data_mode_settings[i].data_mode, CAM_CFG_DATA_MODE_MASK,
		       CAM_CFG_DATA_MODE_SHIFT);

	return data_mode_settings[i].data_mode;
}

/*
 * Moves the completed video buffer from IN-FIFO to the OUT-FIFO after a
 * STOP Interrupt is signalled by the CPI IP.
 */
static void alif_cam_work_helper(const struct device *dev)
{
	enum video_signal_result signal_status = VIDEO_BUF_DONE;
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_buffer *vbuf = NULL;

	if (config->axi_bus_ep) {
		k_mutex_lock(&data->lock, K_FOREVER);

		vbuf = k_fifo_peek_head(&data->fifo_in);
		if (vbuf == NULL) {
			LOG_ERR("Unexpected condition! The IN-FIFO should have "
				"at leastone buffer when we are handling bottom "
				"half of STOP interrupt!");
			data->curr_vid_buf = 0;
			data->is_streaming = false;
			signal_status = VIDEO_BUF_ERROR;
			k_mutex_unlock(&data->lock);
			goto done;
		}

		if (data->curr_vid_buf != (uint32_t)vbuf->buffer) {
			signal_status = VIDEO_BUF_ERROR;
			LOG_ERR("Unknown Video Buffer assigned to CPI Controller.");
			k_mutex_unlock(&data->lock);
			goto done;
		}

		/* Move completed buffer from IN-FIFO to OUT-FIFO. */
		vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT);
		if (!vbuf) {
			LOG_ERR("Failed to get a video buffer from IN-FIFO, "
					"despite IN-FIFO having data");
			data->curr_vid_buf = 0;
			data->is_streaming = false;
			signal_status = VIDEO_BUF_ERROR;
			k_mutex_unlock(&data->lock);
			goto done;
		}

		/* Update the last-update timestamp of buffer. */
		vbuf->timestamp = k_uptime_get_32();

		/* Move finished buffer to OUT-FIFO. */
		k_fifo_put(&data->fifo_out, vbuf);

		/*
		 * Buffer-starvation/resume contract (Alp Lab AB): this IN-FIFO
		 * peek and alif_cam_enqueue()'s fifo_in put + `starved` check run
		 * under the same data->lock, so a buffer racing this pause can
		 * never be lost -- either this peek sees the buffer an in-flight
		 * enqueue() just queued (capture continues below, no pause) or
		 * enqueue() sees `starved` already set once this function
		 * releases the lock (enqueue() then reprograms CAM_FRAME_ADDR and
		 * restarts the endpoint + CPI). Capture must PAUSE here, not stop
		 * for good -- a consumer holding its one buffer past a frame
		 * period used to kill the stream permanently (bench-proven
		 * 2026-09-21).
		 */
		vbuf = k_fifo_peek_head(&data->fifo_in);
		if (vbuf == NULL) {
			LOG_DBG("No more Empty buffers in the IN-FIFO."
					"Pausing Video Capture. Next enqueue() resumes it.");
			data->curr_vid_buf = 0;
			data->starved = true;
			video_stream_stop(config->endpoint_dev, VIDEO_BUF_TYPE_OUTPUT);
			signal_status = VIDEO_BUF_DONE;
			k_mutex_unlock(&data->lock);
			goto done;
		}

		/*
		 * Set the curr_vid_buf to the device here and restart data capture.
		 */
		data->curr_vid_buf = (uint32_t)vbuf->buffer;
		sys_write32(local_to_global(UINT_TO_POINTER(data->curr_vid_buf)),
				regs + CAM_FRAME_ADDR);

		/* Restart video capture. */
		hw_cam_start_video_capture(dev);

		k_mutex_unlock(&data->lock);

done:
		LOG_DBG("cur_vid_buf - 0x%08x", data->curr_vid_buf);
#if defined(CONFIG_POLL)
		if (data->signal) {
			k_poll_signal_raise(data->signal, signal_status);
		}
#endif /* defined(CONFIG_POLL) */
	} else {
		/* Restart video capture. */
		hw_cam_start_video_capture(dev);
	}
}

static void alif_isr_cb_work(struct k_work *work)
{
	struct video_cam_data *data = CONTAINER_OF(work, struct video_cam_data, cb_work);

	alif_cam_work_helper(data->dev);
}

static int alif_cam_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct video_cam_config *config = dev->config;
	int bits_pp = pix_fmt_bpp(fmt->pixelformat);
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_format link = *fmt;
	int mode = config->data_mode;
	int ret;

	/*
	 * Bail out when the bits per pixel is zero (un-supported format).
	 *
	 * v4.4 video-API shim (Alp Lab AB): the fork callback took an
	 * `enum video_endpoint_id ep` (validated VIDEO_EP_OUT / VIDEO_EP_ALL);
	 * v4.4 removed that type and the per-endpoint dispatch, so the param +
	 * its validation branch are dropped.
	 */
	if (!bits_pp) {
		LOG_ERR("Bits-per-pixel - %d", bits_pp);
		return -EINVAL;
	}

	/* An unpacked request (SBGGR10..) goes out on the link as its packed twin. */
	for (size_t i = 0; i < ARRAY_SIZE(cpi_unpacked_fmts); i++) {
		if ((config->interface == CAM_INTERFACE_SERIAL) &&
		    (fmt->pixelformat == cpi_unpacked_fmts[i].mem)) {
			link.pixelformat = cpi_unpacked_fmts[i].link;
		}
	}

	sys_write32((((fmt->height - 1) & CAM_VIDEO_FCFG_ROW_MASK) << CAM_VIDEO_FCFG_ROW_SHIFT) |
			    ((fmt->width & CAM_VIDEO_FCFG_DATA_MASK) << CAM_VIDEO_FCFG_DATA_SHIFT),
		    regs + CAM_VIDEO_FCFG);

	ret = video_set_format(config->endpoint_dev, &link);
	if (ret) {
		LOG_ERR("Failed to set sensor Format.");
		return ret;
	}

	if (config->interface == CAM_INTERFACE_SERIAL) {
		mode = alif_cam_set_csi(dev, link.pixelformat);
		if (mode < 0) {
			LOG_ERR("Failed to configure CAM as per the CSI.");
			return mode;
		}

		ret = alif_cam_sync_pixclk(dev);
		if (ret) {
			return ret;
		}

		/*
		 * ponytail: the 32-bit RGB888 container keeps its RGB24 fourcc
		 * (only the pitch is corrected); remap it once a sensor needs it.
		 */
		fmt->pixelformat = link.pixelformat;
		for (size_t i = 0; i < ARRAY_SIZE(cpi_unpacked_fmts); i++) {
			if (link.pixelformat == cpi_unpacked_fmts[i].link) {
				fmt->pixelformat = cpi_unpacked_fmts[i].mem;
			}
		}
	}

	/*
	 * Alp Lab AB: the CPI stores one data-mode sample per pixel (CSI: the
	 * mode set above; parallel: the DT data-mode, 1..16 bits), whatever the
	 * link bpp -- so 8/16/32-bit modes take 1/2/4 B per pixel.  The caller's
	 * pitch (0 from the portable backend, width * 10 / 8 from a packed-RAW10
	 * sensor) would under-size the enqueue guard's frame and let the DMA run
	 * past the buffer.
	 */
	fmt->pitch = DIV_ROUND_UP(fmt->width << mode, BITS_PER_BYTE);

	data->current_format.pixelformat = fmt->pixelformat;
	data->current_format.pitch = fmt->pitch;
	data->current_format.width = fmt->width;
	data->current_format.height = fmt->height;

	data->is_streaming = 0;
	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its VIDEO_EP_OUT/ALL validation; forwarding helpers lose the `ep` arg.
 */
static int alif_cam_get_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	int ret;

	if (!fmt) {
		return -EINVAL;
	}

	ret = video_get_format(config->endpoint_dev, fmt);
	if (ret) {
		LOG_ERR("Failed to get format from Video pipeline!");
		return ret;
	}

	/* set_fmt also programs the CSI side and rewrites fmt to the in-memory
	 * layout, so no second alif_cam_set_csi() here: it would now see the
	 * unpacked fourcc, which is not a link format.
	 */
	ret = alif_cam_set_fmt(dev, fmt);
	if (ret) {
		return ret;
	}

	fmt->pixelformat = data->current_format.pixelformat;
	fmt->width = data->current_format.width;
	fmt->height = data->current_format.height;
	fmt->pitch = data->current_format.pitch;

	return 0;
}

static int alif_cam_stream_start(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_buffer *vbuf;
	int ret;

	/* Cancel any stale work_helper from previous session before starting */
	struct k_work_sync sync;

	k_work_cancel_sync(&data->cb_work, &sync);

	/* Alp Lab AB: k_work_cancel_sync() above must stay OUTSIDE the lock --
	 * it blocks on alif_cam_work_helper() (which itself takes data->lock)
	 * finishing, so taking data->lock first would deadlock against it. */
	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->is_streaming) {
		LOG_DBG("Already streaming.");
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}

	if (((IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) && config->axi_bus_ep) ||
	    (!IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED))) {
		/* Update the Video-buffer address to CPI-Controller. */
		vbuf = k_fifo_peek_head(&data->fifo_in);
		if (!vbuf) {
			LOG_ERR("No empty video-buffer. Aborting!!!");
			k_mutex_unlock(&data->lock);
			return -ENOBUFS;
		}

		data->curr_vid_buf = (uint32_t)vbuf->buffer;
		sys_write32(local_to_global(UINT_TO_POINTER(data->curr_vid_buf)),
				regs + CAM_FRAME_ADDR);
	}

	/* Setup the interrupts. */
	hw_enable_interrupts(regs, INTR_VSYNC | INTR_BRESP_ERR | INTR_OUTFIFO_OVERRUN |
					   INTR_INFIFO_OVERRUN | INTR_STOP);

	/* Start the MIPI CSI-2 IP in case the MIPI CSI is available. */
	ret = video_stream_start(config->endpoint_dev, VIDEO_BUF_TYPE_OUTPUT);
	if (ret) {
		LOG_ERR("Failed to start streaming of Video pipeline!");
		k_mutex_unlock(&data->lock);
		return -EIO;
	}

	hw_cam_start_video_capture(dev);
	LOG_DBG("Stream started");

	data->is_streaming = true;
	data->starved = false;

	k_mutex_unlock(&data->lock);

	return 0;
}

static int alif_cam_stream_stop(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t mask;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->is_streaming) {
		LOG_DBG("Already stopped streaming.");
		k_mutex_unlock(&data->lock);
		return 0;
	}

	/*
	 * Alp Lab AB: while `starved`, alif_cam_work_helper() already stopped
	 * the endpoint as part of the starvation pause -- stopping it again
	 * here would be a redundant I2C round trip. Clearing `starved` is
	 * what actually matters: it stops a LATER enqueue() from mistaking
	 * this user stop for a starvation pause and auto-restarting the
	 * stream.
	 */
	if (!data->starved) {
		ret = video_stream_stop(config->endpoint_dev, VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to stop streaming in Pipeline!");
			k_mutex_unlock(&data->lock);
			return ret;
		}
	}

	/* Disable Interrupts. */
	hw_disable_interrupts(regs, INTR_VSYNC | INTR_BRESP_ERR | INTR_OUTFIFO_OVERRUN |
					    INTR_INFIFO_OVERRUN | INTR_STOP);

	/* Stop the Camera sensor to dump image. */
	sys_write32(0, regs + CAM_CTRL);

	/* Set the Current buffer state to NULL */
	data->curr_vid_buf = 0;

	/*
	 * Poll on Busy flag of CPI to find out when the video buffer
	 * is no longer accessed.
	 */
	mask = CAM_CTRL_BUSY;
	for (int i = 0; (i < 1000) && (sys_read32(regs + CAM_CTRL) & mask) == mask; i++) {
		k_msleep(1);
	}

	/* Apply soft reset to clear BUSY flag and leave CPI in clean state */
	hw_cam_soft_reset(regs);

	/* Clear any stale latched interrupts after reset */
	sys_write32(sys_read32(regs + CAM_INTR), regs + CAM_INTR);

	LOG_DBG("Stream stopped");

	data->is_streaming = false;
	data->starved = false;

	k_mutex_unlock(&data->lock);

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): set_stream gained an `enum video_buf_type
 * type` param (the CPI is an output-only capture device, so the value is
 * unused); the legacy `bool`-only signature is gone.
 */
static int alif_cam_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(type);

	if (enable) {
		return alif_cam_stream_start(dev);
	} else {
		return alif_cam_stream_stop(dev);
	}
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param.
 */
static int alif_cam_flush(const struct device *dev, bool cancel)
{
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_buffer *vbuf = NULL;
	uint32_t mask;

	if (!cancel) {
		/*
		 * Alp Lab AB: data->lock guards the curr_vid_buf read + the
		 * non-blocking fifo_in drain below -- NOT the wait loop that
		 * follows, which blocks on alif_cam_work_helper() making
		 * progress and would deadlock against it (the helper needs
		 * the same lock) if held across the wait.
		 */
		k_mutex_lock(&data->lock, K_FOREVER);
		if (!data->curr_vid_buf) {
			while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
				k_fifo_put(&data->fifo_out, vbuf);
			}
		}
		k_mutex_unlock(&data->lock);

		/*
		 * In case the cancel option is not provided, put the thread to
		 * sleep for 1 ms repeatedly. On every interrupt, the Video
		 * buffers will move from IN-FIFO to OUT-FIFO.
		 */
		while (!k_fifo_is_empty(&data->fifo_in)) {
			k_msleep(1);
		}
	} else {
		/*
		 * Disable interrupts to ensures that the current buffer is
		 * not moved from IN-FIFO to OUT-FIFO on STOP interrupt.
		 */
		/* Disable the Stop interrupt. */
		hw_disable_interrupts(regs, INTR_STOP);
		/* Stop Video capture. */
		sys_write32(0, regs + CAM_CTRL);

		/*
		 * Poll on Busy flag of CPI to find out when the video buffer
		 * is no longer accessed.
		 */
		mask = CAM_CTRL_BUSY;
		for (int i = 0; (i < 1000) && (sys_read32(regs + CAM_CTRL) & mask) == mask; i++) {
			k_msleep(1);
		}

		/* Apply soft reset to clear BUSY flag and leave CPI in clean state */
		hw_cam_soft_reset(regs);

		k_mutex_lock(&data->lock, K_FOREVER);
		while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
			k_fifo_put(&data->fifo_out, vbuf);
			LOG_DBG("Video Buffer Aborted!!! - 0x%x", (uint32_t)vbuf->buffer);
#if defined(CONFIG_POLL)
			if (data->signal) {
				k_poll_signal_raise(data->signal, VIDEO_BUF_ABORTED);
			}
#endif /* defined(CONFIG_POLL) */
		}
		k_mutex_unlock(&data->lock);
	}

	/* Set current Video buffer to null address. */
	k_mutex_lock(&data->lock, K_FOREVER);
	data->curr_vid_buf = 0;
	k_mutex_unlock(&data->lock);
	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its VIDEO_EP_OUT validation branch.
 */
static int alif_cam_enqueue(const struct device *dev, struct video_buffer *buf)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t to_read;
	uint32_t tmp;
	int ret;

	if (IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) {
		if (!config->axi_bus_ep) {
			/* No AXI memory endpoint on this instance (e.g. ISP-fed CPI,
			 * config->isp_ep) -- enqueueing a buffer here would never be
			 * filled. -ENOTSUP, not 0: a silent "success" would let the
			 * caller believe the buffer is queued. */
			LOG_DBG("AXI Master interface of the IP is not enabled!");
			return -ENOTSUP;
		}
	}

	/* Check if the video buffer is 8-byte aligned or not.*/
	tmp = (uint32_t)buf->buffer;
	if (ROUND_UP(tmp, 8) != tmp) {
		LOG_ERR("Video Buffer is not aligned to 8-byte boundary."
			"It can result in corruption of captured image.");
		return -ENOBUFS;
	}

	to_read = data->current_format.pitch * data->current_format.height;

	/*
	 * The frame has to FIT.  pitch and height come either from the caller's
	 * struct video_format via alif_cam_set_fmt() or from the SENSOR via
	 * video_get_format(), and the buffer address then goes straight to
	 * CAM_FRAME_ADDR for the AXI master to DMA into.  The hardware imposes no
	 * bound of its own -- HWRM 17.1.4.10.1: "The CPI controller hardware can
	 * handle the AXI write crossing the 4KB boundary of AXI address space.
	 * Thus, there is no limitation on software to allocate the memory space for
	 * buffering the image data."
	 *
	 * So a 320x240x2 = 153600-byte pool with a sensor reporting 1280x720 had
	 * 1843200 bytes written starting at the pool block, over whatever the
	 * linker placed after it in SRAM0.  Alignment was validated here and size
	 * was not, while the LOG_DBG below printed both next to each other without
	 * comparing them (#1825).
	 */
	if (to_read > buf->size) {
		LOG_ERR("Frame %ux%u needs %u bytes, buffer holds %u -- refusing to DMA "
		        "past the end",
		        data->current_format.pitch,
		        data->current_format.height,
		        to_read,
		        buf->size);
		return -ENOBUFS;
	}

	buf->bytesused = to_read;

	/*
	 * Buffer-starvation/resume contract (Alp Lab AB): a STOP interrupt
	 * that found the IN-FIFO empty pauses capture instead of stopping it
	 * for good (alif_cam_work_helper() sets `starved` and stops the
	 * endpoint). data->lock serializes the put + `starved` check here
	 * against that empty-check + pause decision, so a buffer racing the
	 * pause is never lost -- see the comment at the work helper.
	 */
	k_mutex_lock(&data->lock, K_FOREVER);

	k_fifo_put(&data->fifo_in, buf);

	if (data->starved) {
		data->curr_vid_buf = (uint32_t)buf->buffer;
		sys_write32(local_to_global(UINT_TO_POINTER(data->curr_vid_buf)),
				regs + CAM_FRAME_ADDR);

		ret = video_stream_start(config->endpoint_dev, VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to resume streaming after buffer starvation! ret - %d",
				ret);
			k_mutex_unlock(&data->lock);
			return ret;
		}

		/* Restart video capture. */
		hw_cam_start_video_capture(dev);
		data->starved = false;
	}

	k_mutex_unlock(&data->lock);

	LOG_DBG("Enqueued buffer: Addr - 0x%x, size - %d, bytesused - %d",
		(uint32_t)buf->buffer, buf->size, buf->bytesused);

	/* Clean+invalidate BEFORE the DMA so no dirty line is written back over the
	 * incoming frame.  The matching post-DMA invalidate is in
	 * alif_cam_dequeue(), without which speculative prefetch during the capture
	 * window can repopulate lines the DMA then overwrites in memory and the
	 * application reads stale pixels (#1825). */
	(void)sys_cache_data_flush_and_invd_range(buf->buffer, buf->size);

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its VIDEO_EP_OUT validation branch.
 */
static int alif_cam_dequeue(const struct device *dev, struct video_buffer **buf,
		       k_timeout_t timeout)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;

	if (IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) {
		if (!config->axi_bus_ep) {
			/* Same no-AXI-endpoint instance as alif_cam_enqueue() above.
			 * -ENOTSUP, *buf left untouched -- this used to return 0
			 * ("success") without ever setting *buf, which a caller
			 * reading it on a 0 return would see as an uninitialized
			 * pointer. */
			LOG_DBG("AXI Master interface of the IP is not enabled!");
			return -ENOTSUP;
		}
	}

	*buf = k_fifo_get(&data->fifo_out, timeout);
	if (!(*buf)) {
		return -EAGAIN;
	}

	/*
	 * Invalidate what the CPI AXI master just wrote.  The enqueue path already
	 * cleans the buffer before the transfer; nothing invalidated it afterwards,
	 * so with CONFIG_DCACHE=y and no nocache placement the application could
	 * read cache lines that speculative prefetch had pulled in during the
	 * capture window -- stale pixels, no error (#1825).
	 * (eth_dwmac_alif_ensemble.c enforces the same requirement with a
	 * BUILD_ASSERT; this driver had no equivalent.)
	 */
	(void)sys_cache_data_invd_range((*buf)->buffer, (*buf)->bytesused);

	LOG_DBG("Dequeued buffer: Addr - 0x%08x, size - %d, bytesused - %d",
		(uint32_t)(*buf)->buffer, (*buf)->size, (*buf)->bytesused);
	return 0;
}

/*
 * v4.4 video-API shim (Alp Lab AB): the CPI controller owns NO controls of its
 * own -- the fork bodies forwarded a raw `void *value` to the endpoint device
 * via the OLD `video_set_ctrl(dev, cid, value)` / `video_get_ctrl(dev, cid,
 * value)` helpers.  v4.4 reshaped those helpers to take a `struct video_control
 * *` whose value is owned by the framework's per-device control registry
 * (video_init_ctrl), so a value-less pass-through forwarder no longer fits the
 * model.  Controls are addressed directly on the endpoint (sensor / CSI)
 * device, so the CPI's forwarding .set_ctrl / .get_ctrl callbacks are dropped
 * from the API table below.
 */

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its VIDEO_EP_OUT/ALL validation; get_caps forwards without `ep`.
 */
static int alif_cam_get_caps(const struct device *dev, struct video_caps *caps)
{
	const struct video_cam_config *config = dev->config;
	int err = -ENODEV;

	err = video_get_caps(config->endpoint_dev, caps);
	caps->min_vbuf_count = CPI_MIN_VBUF;

	return err;
}

#ifdef CONFIG_POLL
/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param.
 */
static int alif_cam_set_signal(const struct device *dev,
			  struct k_poll_signal *signal)
{
	struct video_cam_data *data = dev->data;

	if (signal != NULL && data->signal) {
		return -EALREADY;
	}

	data->signal = signal;
	return 0;
}
#endif

static DEVICE_API(video, cam_driver_api) = {
	.set_format = alif_cam_set_fmt,
	.get_format = alif_cam_get_fmt,
	.set_stream = alif_cam_set_stream,
	.flush = alif_cam_flush,
	.enqueue = alif_cam_enqueue,
	.dequeue = alif_cam_dequeue,
	.get_caps = alif_cam_get_caps,
#ifdef CONFIG_POLL
	.set_signal = alif_cam_set_signal,
#endif
};

static void __maybe_unused alif_video_cam_isr(const struct device *dev)
{
	static bool is_not_corrupted_frame = true;
	struct video_cam_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t err_mask = INTR_OUTFIFO_OVERRUN | INTR_INFIFO_OVERRUN | INTR_BRESP_ERR;
	uint32_t int_st = 0;
	uint32_t tmp = 0;

	/* Clear interrupts. */
	int_st = sys_read32(regs + CAM_INTR) & sys_read32(regs + CAM_INTR_ENA);
	sys_write32(int_st, regs + CAM_INTR);

	if (int_st & INTR_HSYNC) {
		LOG_DBG("H-SYNC detected.");
	}

	if (int_st & INTR_VSYNC) {
		LOG_DBG("V-SYNC detected.");
	}

	if (int_st & INTR_BRESP_ERR) {
		tmp = sys_read32(regs + CAM_AXI_ERR_STAT);
		LOG_ERR("AXI Error count - %ld, AXI BRESP error code - %ld",
			((tmp >> CAM_AXI_ERR_STAT_CNT_SHIFT) & CAM_AXI_ERR_STAT_CNT_MASK),
			((tmp >> CAM_AXI_ERR_STAT_BRESP_SHIFT) & CAM_AXI_ERR_STAT_BRESP_MASK));
	}

	if (int_st & err_mask) {
		LOG_ERR("Frame Capture Error. int_st - 0x%08x", int_st);
		is_not_corrupted_frame = false;
#if defined(CONFIG_POLL)
		if (data->signal) {
			k_poll_signal_raise(data->signal, VIDEO_BUF_ERROR);
		}
#endif /* defined(CONFIG_POLL) */
	}

	if (int_st & INTR_STOP) {
		sys_write32(0, regs + CAM_CTRL);
		/* Guard: don't process stale STOP after stream was turned off */
		if (!data->is_streaming) {
			return;
		}
		/* No corruption observed during dumping this frame. */
		if (is_not_corrupted_frame) {
			LOG_DBG("Video Capture stopped.");
			k_work_submit_to_queue(&data->cb_workq, &data->cb_work);
		} else {
			/* Wait for user to handle corrupted frame capture. */
			data->curr_vid_buf = 0;
			is_not_corrupted_frame = true;
		}
	}
}

static int alif_video_cam_set_config(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	if ((config->data_mode >= CPI_DATA_MODE_16_BIT) && config->is_lpcam) {
		LOG_ERR("LP-CPI controller does not support "
			"any data format >= 16-bit.");
		return -EINVAL;
	}

	if ((config->data_mode >= CPI_DATA_MODE_32_BIT) &&
	    (config->interface == CAM_INTERFACE_PARALLEL)) {
		LOG_ERR("data modes 32-bit and 64-bit are reserved "
			"for CSI-2 only.");
		return -EINVAL;
	}

	/* Setup Camera signals (V-Sync, H-Sync, PCLK) polarity inversion. */
	sys_write32(config->polarity, regs + CAM_CFG);

	/* Capture video-data when both V-Sync and H-Sync are high. */
	if (config->vsync_en) {
		sys_set_bits(regs + CAM_CFG, CAM_CFG_VSYNC_EN);
	} else {
		sys_clear_bits(regs + CAM_CFG, CAM_CFG_VSYNC_EN);
	}

	/* Capture video-data when both V-Sync and H-Sync are aligned. */
	if (config->wait_vsync) {
		sys_set_bits(regs + CAM_CFG, CAM_CFG_WAIT_VSYNC);
	} else {
		sys_clear_bits(regs + CAM_CFG, CAM_CFG_WAIT_VSYNC);
	}

	/* Capture video-data with 10-bits of data on 8-bit channel encoding. */
	if (config->code10on8 && (config->data_mode == CPI_DATA_MODE_8_BIT)) {
		sys_set_bits(regs + CAM_CFG, CAM_CFG_CODE10ON8);
	} else {
		sys_clear_bits(regs + CAM_CFG, CAM_CFG_CODE10ON8);
	}

	/* MSB selection of video-data significant only when data-mode <= 8. */
	if (config->msb && (config->data_mode <= CPI_DATA_MODE_8_BIT)) {
		sys_set_bits(regs + CAM_CFG, CAM_CFG_MSB);
	} else {
		sys_clear_bits(regs + CAM_CFG, CAM_CFG_MSB);
	}

	/* Data-Mask to be set only when Data-Mode(pin-connects) is 16-bit. */
	if (config->data_mode == CPI_DATA_MODE_16_BIT) {
		reg_write_part(regs + CAM_CFG, config->data_mask, CAM_CFG_DATA_MASK,
			       CAM_CFG_DATA_SHIFT);
	}

	/* If write to memory via AXI-bus EP is enabled. */
	if (IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) {
		if (config->axi_bus_ep) {
			sys_set_bits(regs + CAM_CFG, CAM_CFG_AXI_PORT_EN);
		}
	}

	/* If forwarding the video signals to ISP via ISP interface is enabled. */
	if (IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) {
		if (config->isp_ep) {
			sys_set_bits(regs + CAM_CFG, CAM_CFG_ISP_PORT_EN);
		}
	}

	reg_write_part(regs + CAM_CFG, config->data_mode, CAM_CFG_DATA_MODE_MASK,
		       CAM_CFG_DATA_MODE_SHIFT);

	return 0;
}

int alif_video_cam_setup(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t tmp = 0;
	int ret;

	/* Setup the Watermarks. */
	tmp = ((config->write_wmark & CAM_FIFO_CTRL_WR_WMARK_MASK)
	       << CAM_FIFO_CTRL_WR_WMARK_SHIFT) |
	      ((config->read_wmark & CAM_FIFO_CTRL_RD_WMARK_MASK) << CAM_FIFO_CTRL_RD_WMARK_SHIFT);
	sys_write32(tmp, regs + CAM_FIFO_CTRL);

	ret = alif_video_cam_set_config(dev);
	if (ret) {
		LOG_ERR("Setup of CPI controller failed. ret - %d", ret);
		return ret;
	}

	return 0;
}

static int alif_cam_enable_clocks(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;

	/* Enable CAM clock. */
	return clock_control_on(config->clk_dev, config->cid);
}

static int __maybe_unused alif_video_cam_init(const struct device *dev)
{
	const struct video_cam_config *config = dev->config;
	struct video_cam_data *data = dev->data;
	int ret = 0;

	/*
	 * In-case there is no sensor device or CSI controller attached to the
	 * CPI controller, we need to abort.
	 */
	if (!config->endpoint_dev) {
		return -ENODEV;
	}

	if ((config->interface == CAM_INTERFACE_SERIAL) && config->is_lpcam) {
		LOG_ERR("LP-CAM does not support Serial interface!");
		return -EINVAL;
	}

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	LOG_DBG("MMIO Address: 0x%x", (uint32_t)DEVICE_MMIO_GET(dev));

	ret = alif_cam_enable_clocks(dev);
	if (ret) {
		LOG_ERR("CAM clock enable failed! Exiting! ret - %d", ret);
		return ret;
	}

	if (config->pcfg) {
		ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret) {
			LOG_ERR("Failed to apply Pinctrl.");
			return ret;
		}
	}

	/* Setup ISR callback work. */
	k_work_init(&data->cb_work, alif_isr_cb_work);
	k_work_queue_init(&data->cb_workq);
	k_work_queue_start(&data->cb_workq, alif_isr_cb_workq,
			   K_KERNEL_STACK_SIZEOF(alif_isr_cb_workq),
			   K_PRIO_COOP(WORKQ_PRIORITY), NULL);
	k_thread_name_set(&data->cb_workq.thread, "alif_cam_work_helper");

	/* Setup interrupts. */
	config->irq_config_func(dev);

	k_fifo_init(&data->fifo_in);
	k_fifo_init(&data->fifo_out);
	/* Alp Lab AB: guards the buffer-starvation/resume decision shared by
	 * alif_cam_work_helper() and alif_cam_enqueue() (see video_alif.h). */
	k_mutex_init(&data->lock);
	data->dev = dev;

	/* Setup the CPI-Controller hardware config. */
	ret = alif_video_cam_setup(dev);
	if (ret) {
		LOG_ERR("Error setting up the CPI controller.");
		return ret;
	}

	LOG_DBG("irq: %d", config->irq);
	LOG_DBG("Is LP-CPI controller - %d", config->is_lpcam);
	LOG_DBG("Is Parallel interface - %d",
			(config->interface == CAM_INTERFACE_PARALLEL));
	switch (config->data_mode) {
	case CPI_DATA_MODE_1_BIT:
		LOG_DBG("Data-mode: 1-bit");
		break;
	case CPI_DATA_MODE_2_BIT:
		LOG_DBG("Data-mode: 2-bit");
		break;
	case CPI_DATA_MODE_4_BIT:
		LOG_DBG("Data-mode: 4-bit");
		break;
	case CPI_DATA_MODE_8_BIT:
		LOG_DBG("Data-mode: 8-bit");
		break;
	case CPI_DATA_MODE_16_BIT:
		LOG_DBG("Data-mode: 16-bit");
		break;
	case CPI_DATA_MODE_32_BIT:
		LOG_DBG("Data-mode: 32-bit");
		break;
	case CPI_DATA_MODE_64_BIT:
		LOG_DBG("Data-mode: 64-bit");
		break;
	default:
		LOG_DBG("Unknown Data-mode!");
		return -ENODEV;
	}

	if (config->data_mode == CPI_DATA_MODE_16_BIT) {
		switch (config->data_mask) {
		case CPI_DATA_MASK_16_BIT:
			LOG_DBG("Data Mask: 16-bit");
			break;
		case CPI_DATA_MASK_10_BIT:
			LOG_DBG("Data Mask: 10-bit");
			break;
		case CPI_DATA_MASK_12_BIT:
			LOG_DBG("Data Mask: 12-bit");
			break;
		case CPI_DATA_MASK_14_BIT:
			LOG_DBG("Data Mask: 14-bit");
			break;
		default:
			LOG_DBG("Unknown Data mask");
			return -ENODEV;
		}
	}

	if (IS_ENABLED(CONFIG_VIDEO_ALIF_CAM_EXTENDED)) {
		LOG_DBG("AXI EP: %d, ISP EP: %d", config->axi_bus_ep, config->isp_ep);
	}

	data->is_streaming = false;
	data->starved = false;

	return 0;
}

#define REMOTE_DEVICE(i, id) \
	DT_NODE_REMOTE_DEVICE(DT_INST_ENDPOINT_BY_ID(i, id, 0))

/* Alp Lab AB: pix_cid = this CPI's "pix_clk"; csi_pix_cid = the "pix_clk" of
 * the port@0 source (the CSI bridge; a parallel sensor has none). */
#define CAM_GET_CLK(i)                                                         \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(i, clocks),                           \
		(.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(i)),             \
		 .cid = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(i, \
			 cam_clk, clkid),                                      \
		 IF_ENABLED(DT_INST_CLOCKS_HAS_NAME(i, pix_clk),               \
			(.pix_cid = (clock_control_subsys_t)                   \
				DT_INST_CLOCKS_CELL_BY_NAME(i, pix_clk, clkid),)) \
		 IF_ENABLED(DT_CLOCKS_HAS_NAME(REMOTE_DEVICE(i, 0), pix_clk),  \
			(.csi_pix_cid = (clock_control_subsys_t)               \
				DT_CLOCKS_CELL_BY_NAME(REMOTE_DEVICE(i, 0),    \
						       pix_clk, clkid),))))

#define CPI_DEFINE(i)                                                                              \
	IF_ENABLED(CONFIG_PINCTRL,                                                                 \
			(COND_CODE_1(DT_INST_PINCTRL_HAS_IDX(i, 0),                                \
				     (PINCTRL_DT_INST_DEFINE(i);), ())))                           \
	static void cam_config_func_##i(const struct device *dev);                                 \
                                                                                                   \
	static const struct video_cam_config config_##i = {                                        \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(i)),                                              \
                                                                                                   \
		CAM_GET_CLK(i)                                                                     \
		.endpoint_dev = DEVICE_DT_GET(REMOTE_DEVICE(i, 0)),                                \
                                                                                                   \
		.irq = DT_INST_IRQN(i),                                                            \
		.irq_config_func = cam_config_func_##i,                                            \
		IF_ENABLED(CONFIG_PINCTRL,                                                         \
			(COND_CODE_1(DT_INST_PINCTRL_HAS_IDX(i, 0),                                \
				     (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(i),),                 \
				     (.pcfg = NULL,))))                                            \
                                                                                                   \
		.polarity = COND_CODE_1(DT_INST_PROP(i, inv_vsync_pol), CAM_CFG_VSYNC_POL, (0)) |  \
			    COND_CODE_1(DT_INST_PROP(i, inv_hsync_pol), CAM_CFG_HSYNC_POL, (0)) |  \
			    COND_CODE_1(DT_INST_PROP(i, inv_pclk_pol), CAM_CFG_PCLK_POL, (0)),     \
		.read_wmark = DT_INST_PROP(i, fifo_rd_wmark),                                      \
		.write_wmark = DT_INST_PROP(i, fifo_wr_wmark),                                     \
		.is_lpcam = DT_INST_PROP(i, lp_cam),                                               \
		.msb = DT_INST_PROP(i, msb),                                                       \
		.vsync_en = DT_INST_PROP(i, vsync_en),                                             \
		.wait_vsync = DT_INST_PROP(i, wait_vsync),                                         \
		.capture_mode = DT_INST_ENUM_IDX(i, capture_mode),                                 \
		.data_mode = DT_INST_ENUM_IDX(i, data_mode),                                       \
		.data_mask = DT_INST_ENUM_IDX(i, data_mask),                                       \
		.code10on8 = DT_INST_PROP(i, code_10_on_8),                                        \
		.axi_bus_ep = DT_NODE_EXISTS(DT_INST_PORT_BY_ID(i, 1)),                            \
		.isp_ep = DT_NODE_HAS_STATUS_OKAY(                                                 \
				DT_NODE_REMOTE_DEVICE(                                             \
					DT_INST_ENDPOINT_BY_ID(i, 2, 0))),                         \
		.csi_halt_en = DT_INST_PROP(i, csi_halt_en),                                       \
		.interface = DT_INST_ENUM_IDX(i, cpi_interface),                                   \
	};                                                                                         \
                                                                                                   \
	static struct video_cam_data data_##i;                                                     \
	DEVICE_DT_INST_DEFINE(i, &alif_video_cam_init, NULL, &data_##i, &config_##i,               \
			      POST_KERNEL, CONFIG_VIDEO_ALIF_CAM_INIT_PRIORITY, &cam_driver_api);  \
                                                                                                   \
	static void cam_config_func_##i(const struct device *dev)                                  \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(i), DT_INST_IRQ(i, priority), alif_video_cam_isr,         \
			    DEVICE_DT_INST_GET(i), 0);                                             \
		irq_enable(DT_INST_IRQN(i));                                                       \
	}

DT_INST_FOREACH_STATUS_OKAY(CPI_DEFINE)
