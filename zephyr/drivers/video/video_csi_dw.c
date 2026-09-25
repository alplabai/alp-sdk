/*
 * Copyright (C) 2024 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM) ======
 * The DesignWare MIPI CSI-2 host bridge in front of the Alif Ensemble CPI is
 * driven by a vendored copy of the Apache-2.0 zephyr_alif fork driver
 * (drivers/video/video_csi_dw.c, compatible "snps,designware-csi").  Upstream
 * Zephyr v4.4 + hal_alif ship NO DesignWare-CSI video driver, so this is a
 * genuine fork-driver copy carried in-tree so it survives a `west update`.
 * Retire onto the opt-in sdk-alif fork compatible once the csi nodes are
 * repointed AND bench-verified.
 * ==================================================================
 *
 * Vendored from the fork, then PORTED to the upstream Zephyr v4.4 video API by
 * Alp Lab AB.  Like video_alif.c the fork body implemented the OLD
 * `enum video_endpoint_id`-based video API that v4.4 removed.  v4.4 deltas
 * applied here (marked "v4.4 video-API shim (Alp Lab AB)" at each site):
 *   - dropped the `enum video_endpoint_id ep` param + its validation from
 *     set_format/get_format/get_caps; the sensor forwarders lose their `ep`;
 *   - set_stream(dev, bool) gained an `enum video_buf_type type` param;
 *     video_stream_start/_stop now take VIDEO_BUF_TYPE_OUTPUT;
 *   - the value-pointer .set_ctrl/.get_ctrl callbacks (which handled the
 *     PRIVATE CIDs VIDEO_CID_ALIF_CSI_DPHY_FREQ / _CURR_CAM and forwarded to
 *     the sensor) are dropped -- v4.4 controls are registry-owned; see the note
 *     above the API table.  The multi-camera D-PHY-freq / camera-select path is
 *     deferred (BENCH-UNVERIFIED; single-sensor capture does not need it).
 * It depends on the vendored MIPI-DPHY driver
 * (zephyr/drivers/mipi_dphy/dphy_dw.c) for dphy_dw_slave_setup +
 * struct dphy_csi2_settings.  The driver now COMPILES against v4.4 (the
 * ALP_VIDEO_ALIF_BROKEN gate is retired).  vendor-ext, BENCH-UNVERIFIED.
 */
#define DT_DRV_COMPAT snps_designware_csi

#include <zephyr/devicetree.h>

#include <zephyr/sys/device_mmio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/video.h>
#include "video_csi_dw.h"
#include <zephyr/drivers/mipi_dphy/dphy_dw.h>
#include <zephyr/drivers/video/video_alif.h>
/* Upstream's private drivers/video/video_device.h (put on the include path by
 * zephyr/CMakeLists.txt's ${ZEPHYR_BASE}/drivers/video dir) -- needed for
 * VIDEO_DEVICE_DEFINE, below, so v4.4's control-registry walk
 * (video_find_ctrl(), drivers/video/video_ctrls.c) can chain from this
 * device to its upstream sensor. */
#include "video_device.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(csi2_dw, CONFIG_VIDEO_LOG_LEVEL);

static int csi2_is_format_supported(uint32_t fourcc)
{
	/* TODO: Add support for RGB formats. */
	switch (fourcc) {
	case VIDEO_PIX_FMT_Y6P:
	case VIDEO_PIX_FMT_Y7P:
	case VIDEO_PIX_FMT_GREY:
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_SBGGR10P:
	case VIDEO_PIX_FMT_SGBRG10P:
	case VIDEO_PIX_FMT_SGRBG10P:
	case VIDEO_PIX_FMT_SRGGB10P:
	case VIDEO_PIX_FMT_Y12P:
	case VIDEO_PIX_FMT_Y14P:
	case VIDEO_PIX_FMT_Y16:
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
	case VIDEO_PIX_FMT_RGB565:
		return true;
	default:
		return false;
	}
}

static int32_t fourcc_to_csi_data_type(uint32_t fourcc)
{
	/* TODO: Add support for RGB formats. */
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
	}
	return -ENOTSUP;
}

static void reg_write_part(uintptr_t reg, uint32_t data, uint32_t mask, uint8_t shift)
{
	uint32_t tmp = 0;

	tmp = sys_read32(reg);
	tmp &= ~(mask << shift);
	tmp |= (data & mask) << shift;
	sys_write32(tmp, reg);
}

/*
 * v4.4 video-API shim (Alp Lab AB): mask every CSI_INT_MSK_* source to 0
 * (all-disabled). Called from csi2_dw_init() BEFORE config->irq_config_func()
 * arms the NVIC line, so the interrupt cannot fire from a stale/reset
 * register state while the sensor is still unparked.
 *
 * Root cause (bench run 72, E1M-AEN803 2026W36-0001, confirmed against a
 * zeroed ram_console_buf): the pre-banner "Fatal Interrupt caused by PHY due
 * to TX errors" / "PHY Packet discard" lines are genuine POST_KERNEL boot
 * output, not stale RAM. init priorities: MIPI-DPHY 39, this CSI-2 host 41
 * (CONFIG_VIDEO_MIPI_CSI2_DW_INIT_PRIORITY, zephyr/kconfigs/
 * vendor-alif-peripherals.kconfig:856) -- both run before I2C
 * (I2C_INIT_PRIORITY default KERNEL_INIT_PRIORITY_DEVICE=50,
 * <zephyr>/kernel/Kconfig.device:70-72) and well before OV5647's
 * own init, which performs the LP-11 lane park (CONFIG_VIDEO_INIT_PRIORITY
 * default 60, <zephyr>/drivers/video/Kconfig:20-22; the park
 * itself is ov5647.c's documented "DIVERGENCE #1", see ov5647_init()'s
 * header comment). Previously csi2_dw_init() left every CSI_INT_MSK_*
 * register at its power-on-reset value and only unmasked sources later, from
 * csi2_dw_configure() (via csi2_dw_irq_on() below) -- i.e. well after
 * main() starts. In between, the NVIC line was already live (priority 41)
 * while the D-PHY (39) was watching an OV5647 whose lanes were not yet
 * parked to LP-11 (60): any transient/reset-state PHY event in that window
 * could propagate straight to the ISR. Masking explicitly here removes the
 * dependency on the IP's reset-default mask state entirely -- correct
 * regardless of what that default turns out to be. csi2_dw_irq_on() re-masks
 * (unmasks) the real set once the app actually configures the stream, so
 * this changes nothing about steady-state behavior.
 */
static void csi2_dw_irq_off(uintptr_t regs)
{
	sys_write32(0, regs + CSI_INT_MSK_PHY_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_PKT_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_PHY);
	sys_write32(0, regs + CSI_INT_MSK_LINE);
	sys_write32(0, regs + CSI_INT_MSK_IPI_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_BNDRY_FRAME_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_SEQ_FRAME_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_CRC_FRAME_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_PLD_CRC_FATAL);
	sys_write32(0, regs + CSI_INT_MSK_DATA_ID);
	sys_write32(0, regs + CSI_INT_MSK_ECC_CORRECT);
}

static void csi2_dw_irq_on(uintptr_t regs, struct csi2_dw_data *data)
{
	/*
	 * Alp Lab AB: fresh unmask, fresh IPI-fatal WINDOW count -- see
	 * CSI2_DW_IPI_FATAL_LOG_LIMIT. Called from csi2_dw_configure() (set_format time) AND
	 * from csi2_dw_stream_start() (every stream (re)start, below) so a restart always gets
	 * its own cap window; ipi_fatal_total (video_csi_dw.h) is the one NOT reset here.
	 */
	data->ipi_fatal_count = 0;

	/*
	 * Review round (post-3511cd180): the CSI_INT_ST_* registers are
	 * read-to-clear (csi2_dw_irq() reads each one to decode which event
	 * fired, same as CSI_INT_ST_MAIN itself). Between csi2_dw_init()'s
	 * mask-before-arm (csi2_dw_irq_off(), above -- fixed for the
	 * pre-sensor-park spurious-event window) and this function actually
	 * unmasking the real sources, ANY transient event the D-PHY/CSI-2
	 * host saw would still be sitting latched here, ready to fire the
	 * instant the corresponding mask bit goes live below. Read (and
	 * discard) every status register first so unmasking starts from a
	 * clean slate.
	 */
	(void)sys_read32(regs + CSI_INT_ST_MAIN);
	(void)sys_read32(regs + CSI_INT_ST_PHY_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_PKT_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_PHY);
	(void)sys_read32(regs + CSI_INT_ST_LINE);
	(void)sys_read32(regs + CSI_INT_ST_IPI_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_BNDRY_FRAME_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_SEQ_FRAME_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_CRC_FRAME_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_PLD_CRC_FATAL);
	(void)sys_read32(regs + CSI_INT_ST_DATA_ID);
	(void)sys_read32(regs + CSI_INT_ST_ECC_CORRECT);

	sys_write32(INT_PHY_FATAL_MASK, regs + CSI_INT_MSK_PHY_FATAL);
	sys_write32(INT_PKT_FATAL_MASK, regs + CSI_INT_MSK_PKT_FATAL);
	sys_write32(INT_PHY_MASK, regs + CSI_INT_MSK_PHY);
	sys_write32(INT_LINE_ERR_MASK, regs + CSI_INT_MSK_LINE);
	sys_write32(INT_IPI_MASK, regs + CSI_INT_MSK_IPI_FATAL);
	sys_write32(INT_BNDRY_FRAME_FATAL_MASK, regs + CSI_INT_MSK_BNDRY_FRAME_FATAL);
	sys_write32(INT_SEQ_FRAME_FATAL_MASK, regs + CSI_INT_MSK_SEQ_FRAME_FATAL);
	sys_write32(INT_CRC_FRAME_FATAL_MASK, regs + CSI_INT_MSK_CRC_FRAME_FATAL);
	sys_write32(INT_PLD_CRC_FATAL_MASK, regs + CSI_INT_MSK_PLD_CRC_FATAL);
	sys_write32(INT_DATA_ID_MASK, regs + CSI_INT_MSK_DATA_ID);
	sys_write32(INT_ECC_CORRECT_MASK, regs + CSI_INT_MSK_ECC_CORRECT);
}

static void csi2_dw_irq(const struct device *dev)
{
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t global_st = 0;
	uint32_t event_st = 0;
	bool reset_ipi = false;

	global_st = sys_read32(regs + CSI_INT_ST_MAIN);
	if (global_st & CSI_INT_ST_MAIN_IPI_FATAL) {
		event_st = sys_read32(regs + CSI_INT_ST_IPI_FATAL);
		data->ipi_fatal_count++;
		data->ipi_fatal_total++;

		/*
		 * Alp Lab AB: a sensor stuck emitting bad IPI framing fires this once per line --
		 * uncapped LOG_ERR + IPI soft-reset floods the log and storms the reset line (see
		 * CSI2_DW_IPI_FATAL_LOG_LIMIT). Log + reset only the first
		 * CSI2_DW_IPI_FATAL_LOG_LIMIT occurrences of the CURRENT unmask window, then mask
		 * the source and log once that it went quiet. ipi_fatal_count (this window) resets
		 * on the next csi2_dw_irq_on() -- now called from every csi2_dw_stream_start(), not
		 * just the first set_format -- so masking from a previous stream no longer hides a
		 * later stream's own overflow; ipi_fatal_total (video_csi_dw.h) is never reset, so
		 * a later diagnostic dump can still report the true lifetime total.
		 */
		if (data->ipi_fatal_count <= CSI2_DW_IPI_FATAL_LOG_LIMIT) {
			LOG_ERR("Fatal Interrupt at IPI interface. status - 0x%x", event_st);
			reset_ipi = true;
		}
		if (data->ipi_fatal_count == CSI2_DW_IPI_FATAL_LOG_LIMIT) {
			LOG_ERR("IPI fatal interrupts masked after %d (total %u since driver init)",
				CSI2_DW_IPI_FATAL_LOG_LIMIT, data->ipi_fatal_total);
			sys_write32(0, regs + CSI_INT_MSK_IPI_FATAL);
		}
	}
	if (global_st & CSI_INT_ST_MAIN_LINE) {
		event_st = sys_read32(regs + CSI_INT_ST_LINE);
		LOG_ERR("Interrupt due to error in Line construction. "
			"status - 0x%x",
			event_st);
	}
	if (global_st & CSI_INT_ST_MAIN_PHY) {
		event_st = sys_read32(regs + CSI_INT_ST_PHY);
		LOG_ERR("Fatal Interrupt caused by PHY due to TX errors. "
			"status - 0x%x",
			event_st);
	}
	if (global_st & CSI_INT_ST_MAIN_ECC_CORRECTED) {
		event_st = sys_read32(regs + CSI_INT_ST_ECC_CORRECT);
		LOG_ERR("Interrupt for header error detection and correction "
			"for specific VC-ID. status - 0x%x",
			event_st);
	}
	if (global_st & CSI_INT_ST_MAIN_DATA_ID) {
		event_st = sys_read32(regs + CSI_INT_ST_DATA_ID);
		LOG_ERR("Interrupt due to unknown data type detected in a "
			"specific VC. Packet discarded. status - 0x%x",
			event_st);
	}
	if (global_st & CSI_INT_ST_MAIN_PLD_CRC) {
		event_st = sys_read32(regs + CSI_INT_ST_PLD_CRC_FATAL);
		LOG_ERR("Fatal Interrupt due to payload checksum error."
			"status - 0x%x",
			event_st);
		reset_ipi = true;
	}
	if (global_st & CSI_INT_ST_MAIN_FRAME_CRC) {
		event_st = sys_read32(regs + CSI_INT_ST_CRC_FRAME_FATAL);
		LOG_ERR("Fatal Interrupt due to frames with at least one CRC "
			"error. status - 0x%x",
			event_st);
		reset_ipi = true;
	}
	if (global_st & CSI_INT_ST_MAIN_FRAME_SEQ) {
		event_st = sys_read32(regs + CSI_INT_ST_SEQ_FRAME_FATAL);
		LOG_ERR("Fatal Interrupt due to incorrect frame sequence for "
			"a specific VC. status - 0x%x",
			event_st);
		reset_ipi = true;
	}
	if (global_st & CSI_INT_ST_MAIN_FRAME_BNDRY) {
		event_st = sys_read32(regs + CSI_INT_ST_BNDRY_FRAME_FATAL);
		LOG_ERR("Fatal Interrupt due to mismatch of Frame Start and "
			"Frame End for a specific VC. status - 0x%x",
			event_st);
		reset_ipi = true;
	}
	if (global_st & CSI_INT_ST_MAIN_PKT) {
		event_st = sys_read32(regs + CSI_INT_ST_PKT_FATAL);
		LOG_ERR("Fatal Interrupt related to Packet construction. "
			"Packet discarded. status - 0x%x",
			event_st);
		reset_ipi = true;
	}
	if (global_st & CSI_INT_ST_MAIN_PHY_FATAL) {
		event_st = sys_read32(regs + CSI_INT_ST_PHY_FATAL);
		LOG_ERR("Fatal Interrupt due to PHY Packet discard. "
			"status - 0x%x",
			event_st);
		reset_ipi = true;
	}

	if (reset_ipi) {
		LOG_ERR("Review the Timings programmed to IPI. "
			"Resetting the IPI for now.");
		sys_clear_bits(regs + CSI_IPI_SOFTRSTN, CSI_IPI_SOFTRSTN_RSTN);
		sys_set_bits(regs + CSI_IPI_SOFTRSTN, CSI_IPI_SOFTRSTN_RSTN);
	}
}

static int csi2_dw_ipi_advanced_features(const struct device *dev)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/*
	 * 1. Disable Frame start to trigger any sync event.
	 * 2. Enable Manual selection of packets for Line Delimiters.
	 * 3. Disable use of embedded packets for IPI sync events.
	 * 4. Disable use of blanking packets for IPI sync events.
	 * 5. Disable use of NULL packets for IPI sync events.
	 * 6. Disable use of line start packets for IPI sync events.
	 * 7. Enable video packets for IPI sync events
	 * 8. Disable IPI Data Type overwrite.
	 */
	sys_clear_bits(regs + CSI_IPI_ADV_FEATURES,
		       CSI_IPI_ADV_FEATURES_SYNC_EVENT | CSI_IPI_ADV_FEATURES_EN_EMBEDDED |
			       CSI_IPI_ADV_FEATURES_EN_BLANKING | CSI_IPI_ADV_FEATURES_EN_NULL |
			       CSI_IPI_ADV_FEATURES_EN_LINE_START |
			       CSI_IPI_ADV_FEATURES_DT_OVERWRITE);

	sys_set_bits(regs + CSI_IPI_ADV_FEATURES,
		     CSI_IPI_ADV_FEATURES_SEL_LINE_EVENT | CSI_IPI_ADV_FEATURES_EN_VIDEO);

	return 0;
}

static int csi2_dw_ipi_set_timings(const struct device *dev)
{
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct timings *timing =
		&data->time[data->current_sensor];
	uint32_t tmp;

	tmp = timing->hsa + timing->hbp + timing->hsd + timing->hact;

	/* Horizontal timing. */
	sys_write32(timing->hsa & CSI_IPI_HSA_TIME_MASK, regs + CSI_IPI_HSA_TIME);
	sys_write32(timing->hbp & CSI_IPI_HBP_TIME_MASK, regs + CSI_IPI_HBP_TIME);
	sys_write32(timing->hsd & CSI_IPI_HSD_TIME_MASK, regs + CSI_IPI_HSD_TIME);
	sys_write32(tmp & CSI_IPI_HLINE_TIME_MASK, regs + CSI_IPI_HLINE_TIME);

	/* Vertical timing. */
	sys_write32(timing->vsa & CSI_IPI_VSA_LINES_MASK, regs + CSI_IPI_VSA_LINES);
	sys_write32(timing->vbp & CSI_IPI_VBP_LINES_MASK, regs + CSI_IPI_VBP_LINES);
	sys_write32(timing->vfp & CSI_IPI_VFP_LINES_MASK, regs + CSI_IPI_VFP_LINES);
	sys_write32(timing->vact & CSI_IPI_VACTIVE_LINES_MASK, regs + CSI_IPI_VACTIVE_LINES);

	return 0;
}

static int csi2_dw_ipi_mode_config(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/* Setup IPI mode timings. */
	if (config->ipi_mode == CSI2_IPI_MODE_TIMINGS_CTRL) {
		sys_set_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_MODE);
	} else {
		sys_clear_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_MODE);
	}

	/* Setup IPI interface type. */
	if (data->csi_cpi_settings[data->current_sensor]->ipi_ifx ==
			CSI2_IPI_MODE_16_BIT_IFX) {
		sys_set_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_COLOR_COM);
	} else {
		sys_clear_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_COLOR_COM);
	}

	sys_set_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_CUT_THROUGH);
	return 0;
}

static int csi2_dw_config_host(const struct device *dev)
{
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct dphy_csi2_settings *phy =
		&data->phy[data->current_sensor];
	/*
	 * Configuring the MIPI CSI-2 Host.
	 */
	/* Setup the number of data-lanes. */
	sys_write32(phy->num_lanes - 1, regs + CSI_N_LANES);

	/* Enable Interrupts. */
	csi2_dw_irq_on(regs, data);

	/*
	 * Configuring IPI.
	 */
	/* IPI Mode Configuration. */
	csi2_dw_ipi_mode_config(dev);

	/* Enable Auto memory flush of CSI by default. */
	sys_set_bits(regs + CSI_IPI_MEM_FLUSH, CSI_IPI_MEM_FLUSH_AUTO_FLUSH);

	/* Setup IPI VC-ID. */
	reg_write_part(regs + CSI_IPI_VCID, 0, CSI_IPI_VCID_VCID_MASK, CSI_IPI_VCID_VCID_SHIFT);

	/* Setup IPI Data Type. */
	reg_write_part(regs + CSI_IPI_DATA_TYPE,
		       data->csi_cpi_settings[data->current_sensor]->dt,
		       CSI_IPI_DATA_TYPE_TYPE_MASK, CSI_IPI_DATA_TYPE_TYPE_SHIFT);

	/* Setup IPI Advanced Features. */
	csi2_dw_ipi_advanced_features(dev);

	/* Setup IPI timings. */
	csi2_dw_ipi_set_timings(dev);

	return 0;
}

static int csi2_dw_phy_config(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	int ret;

	ret = dphy_dw_slave_setup(config->rx_dphy, &data->phy[data->current_sensor],
			data->current_sensor);
	if (ret) {
		/* dphy_dw_slave_setup(): id 0 = dedicated CSI RX D-PHY, id 1 = DSI TX
		 * D-PHY in RX mode (the fork label had the two swapped).
		 */
		LOG_ERR("Failed to set-up D-PHY %s", (data->current_sensor ? "TX as RX" : "RX"));
		return ret;
	}
	return 0;
}

static int csi2_dw_validate_data(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	struct timings *timing =
		&data->time[data->current_sensor];
	struct dphy_csi2_settings *phy =
		&data->phy[data->current_sensor];
	float pixclock;
	float pixrate;
	uint32_t bpp;
	uint32_t tmp;
	int ret;

	bpp = data->csi_cpi_settings[data->current_sensor]->bits_per_pixel;
	/*
	 * When camera through-put is slower than IPI, all data is transferred
	 * before new Horizontal Line is received. RAM only needs to store one
	 * line.
	 */
	if (!((timing->hact * bpp / CSI2_HOST_IPI_DWIDTH) <= CSI2_IPI_FIFO_DEPTH)) {
		LOG_ERR("Camera through-put is higher than IPI. "
			"New H-Line causes corruption to stored data.");
		return -EINVAL;
	}

	/*
	 * Balancing bandwidth by making output bandwidth 20% more than input
	 * bandwidth.
	 * pix_clk = ((rx_lane_clock_"ddr" * 2) * num_lanes)/
	 *				bits_per_pixel
	 * Balanced pixel clock for 20% more input bandwidth:
	 * balanced pixel clock = pix_clk * 1.2
	 */
	pixrate = ((float)(phy->pll_fin << 1) * phy->num_lanes) / bpp;

	if (config->ipi_mode == CSI2_IPI_MODE_TIMINGS_CTRL) {
		/*
		 * Alp Lab AB (issue #2287): the 20% margin above exists to keep
		 * the IPI drain rate safely ahead of the sensor in Camera mode,
		 * where HSD is DERIVED from the programmed pixel clock
		 * (csi2_dw_validate_data()'s CAM branch, below). In Controller
		 * mode the causality is reversed: the shield overlay's fixed
		 * csi-hsd was bench-swept against whatever clock the BARE
		 * pixrate request actually lands on, so requesting a margined
		 * (faster) clock here would silently invalidate that overlay's
		 * derivation instead of just missing a margin. Request the bare
		 * rate and let the DT HLINE/VTOTAL set the drain rate.
		 *
		 * Bench evidence (E1M-AEN803 2026W36-0001, IMX296, pll_fin
		 * 594000000, bpp 10 -> pixrate 118800000): requesting the bare
		 * 118.8 MHz landed CSI_PIXCLK_CTRL (0x4903f008) on 0x00030001
		 * (div 3) = 400 MHz / 3 = 133.33 MHz -- the value the shield
		 * overlay's csi-hsd derivation assumes. Requesting the margined
		 * 142.56 MHz instead is CALCULATED (not bench-observed -- this is
		 * exactly the request this branch exists to avoid) to land on div
		 * 2 = 200 MHz per the same clock-control divisor rule: IPI line
		 * time would become 1972 / 200 MHz = 9.86 us, well under the
		 * sensor's 14.815 us line time, i.e. the IPI would starve waiting
		 * on a sensor that cannot keep up -- not the FIFO-overflow failure
		 * margin mode guards against, but just as unusable.
		 */
		pixclock = pixrate;
	} else {
		pixclock = pixrate * (float)CSI2_BANDWIDTH_SCALER;
	}
	LOG_DBG("pll_fin - %d, Check pixclock = %d (CSI_PIXCLK_CTRL)", phy->pll_fin,
		(uint32_t)pixclock);

	tmp = (uint32_t)pixclock;
	ret = clock_control_set_rate(config->clk_dev, config->pixclk,
			(clock_control_subsys_rate_t)tmp);
	if (ret == -ERANGE) {
		/*
		 * Alp Lab AB: the 20 % margin does not fit under the pixel-clock
		 * divider's maximum.  Ask for the bare pixel rate instead: the
		 * clock controller rounds up to the nearest reachable rate, which
		 * is then the maximum.  Fail only if even that does not fit.
		 */
		ret = clock_control_set_rate(config->clk_dev, config->pixclk,
				(clock_control_subsys_rate_t)(uint32_t)pixrate);
		if (ret == 0) {
			LOG_WRN("CSI pixclk %u Hz (1.2 x %u Hz pixel rate) exceeds the max; "
				"running at the max with < 20%% margin", tmp, (uint32_t)pixrate);
		} else if (ret == -ERANGE) {
			LOG_ERR("CSI pixel rate %u Hz (link %u Hz, %u lanes, %u bpp) exceeds "
				"the pixel-clock max; use a wider format (e.g. RAW10, not RAW8) "
				"or a lower sensor link frequency", (uint32_t)pixrate,
				phy->pll_fin, phy->num_lanes, bpp);
			return ret;
		}
	}
	if (ret) {
		LOG_ERR("Failed to set CSI pixel clock rate! ret - %d", ret);
		return ret;
	}

	/*
	 * Alp Lab AB: enable only after the divisor and CLK_SEL are programmed
	 * (the DFP set_csi_pixel_clk() order), never on the reset divisor.
	 */
	ret = clock_control_on(config->clk_dev, config->pixclk);
	if (ret) {
		LOG_ERR("Failed to enable CSI pixel clock! ret - %d", ret);
		return ret;
	}

	/* Use the rate actually programmed (a divider rounds up) for the timings. */
	if (clock_control_get_rate(config->clk_dev, config->pixclk, &tmp) == 0) {
		pixclock = tmp;
	}

	if (config->ipi_mode == CSI2_IPI_MODE_TIMINGS_CTRL) {
		/*
		 * Alp Lab AB (issue #2287): log the IPI line time this ACTUAL
		 * programmed clock produces (not the pixclock this branch
		 * requested above) against the DT-fixed HLINE, so a future
		 * clock-control divisor-policy change that lands on a different
		 * divisor -- silently invalidating the overlay's HSD derivation,
		 * see raspberry_pi_global_shutter_camera.overlay -- shows up in
		 * the boot log instead of only as a re-appeared FIFO overflow.
		 */
		uint32_t hline = timing->hsa + timing->hbp + timing->hsd + timing->hact;

		LOG_INF("CSI IPI Controller-mode: pixclk %u Hz, HLINE %u px -> line time %u ns",
			tmp, hline, (uint32_t)(((uint64_t)hline * 1000000000ULL) / tmp));
	}

	if (config->ipi_mode == CSI2_IPI_MODE_TIMINGS_CAM) {
		/*
		 * FV(VSYNC) comes at least 3 pixel clocks before
		 * LV(HSYNC/DATA_EN). Hence, setting HSA as 3.
		 */
		timing->hsa = 3;
		timing->hbp = 0;

		/*
		 * HSD should be such that when the PPI interface should
		 * collect last pixel and send to memory before IPI interface
		 * is collecting the last pixel of Horizontal Active Area.
		 */
		timing->hsd = ((pixclock * bpp * timing->hact) / (phy->pll_fin << 1)) -
			    (timing->hact + timing->hsa) + 1;
		timing->vsa = 0;
		timing->vbp = 0;
		timing->vfp = 0;
		timing->vact = 0;
	}
	return 0;
}

static int csi2_dw_configure(const struct device *dev)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	int ret;

	/* Enter the CSI-2 reset state. */
	sys_write32(0, regs + CSI_CSI2_RESETN);

	ret = csi2_dw_validate_data(dev);
	if (ret) {
		LOG_ERR("Invalid parameters set for CSI-2");
		return ret;
	}

	/* Setup D-PHY */
	ret = csi2_dw_phy_config(dev);
	if (ret) {
		LOG_ERR("Failed to configure PHY.");
		return ret;
	}

	ret = csi2_dw_config_host(dev);
	if (ret) {
		LOG_ERR("Failed to configure CSI Host.");
		return ret;
	}

	/* Exit CSI-2 reset state*/
	sys_write32(1, regs + CSI_CSI2_RESETN);

	return 0;
}

static int csi2_dw_stream_start(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	int ret = -ENODEV;

	if (data->streaming_map & BIT(data->current_sensor)) {
		LOG_DBG("Already Streaming.");
		return 0;
	}

	/*
	 * Alp Lab AB (issue #2287): re-arm the IPI-fatal mask/count here, not just once from
	 * csi2_dw_configure() (set_format time). csi2_dw_irq_on() used to run ONLY from
	 * set_format, so once CSI2_DW_IPI_FATAL_LOG_LIMIT startup transients masked
	 * CSI_INT_MSK_IPI_FATAL, every later real overflow on this same configured stream went
	 * silent -- no LOG_ERR, no soft-reset, and the frame just corrupts quietly. Re-arming on
	 * every stream (re)start bounds the blind window to "at most LOG_LIMIT events since THIS
	 * start", which is the guarantee the masking scheme is supposed to give.
	 */
	csi2_dw_irq_on(regs, data);

	/* Enable CSI streaming */
	sys_set_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_ENABLE);
	LOG_DBG("Stream started");

	/* Enable CMOS sensor streaming */
	if (config->sensor[data->current_sensor]) {
		ret = video_stream_start(config->sensor[data->current_sensor],
					 VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to start sensor stream!");
			return ret;
		}
	} else {
		LOG_ERR("Incorrect sensor selected!");
		return -ENODEV;
	}

	data->streaming_map |= BIT(data->current_sensor);

	return 0;
}

static int csi2_dw_stream_stop(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	int ret = -ENODEV;

	if (!(data->streaming_map & BIT(data->current_sensor))) {
		LOG_DBG("Already Stopped current sensor.");
		if (!data->streaming_map) {
			LOG_DBG("CSI has already stopped streaming.");
		}
		return 0;
	}

	if (!(data->streaming_map & ~BIT(data->current_sensor))) {
		LOG_DBG("Stream stopped from IPI");
		/* Disable CSI streaming */
		sys_clear_bits(regs + CSI_IPI_MODE, CSI_IPI_MODE_ENABLE);
	}

	/* Disable CMOS sensor streaming */
	if (config->sensor[data->current_sensor]) {
		ret = video_stream_stop(config->sensor[data->current_sensor],
					VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to stop sensor stream!");
			return ret;
		}
	} else {
		LOG_ERR("Incorrect sensor selected!");
		return -ENODEV;
	}

	data->streaming_map &= ~BIT(data->current_sensor);

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): set_stream gained an `enum video_buf_type
 * type` param (the CSI-2 bridge is output-only, so the value is unused).
 */
static int csi2_dw_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(type);

	if (enable) {
		return csi2_dw_stream_start(dev);
	} else {
		return csi2_dw_stream_stop(dev);
	}
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param; the sensor-format forwarder loses its `ep` arg.
 */
static int csi2_dw_set_format(const struct device *dev, struct video_format *fmt)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	int64_t link_freq;
	int32_t tmp;
	int ret;
	int i;

	if (config->sensor[data->current_sensor]) {
		ret = video_set_format(config->sensor[data->current_sensor], fmt);
		if (ret) {
			LOG_ERR("Failed to set Sensor pixel format!");
			return ret;
		}
	} else {
		LOG_ERR("Invalid sesnor selected!");
		return -ENODEV;
	}

	if (!csi2_is_format_supported(fmt->pixelformat)) {
		LOG_ERR("FourCC format not supported.");
		return -ENOTSUP;
	}

	/*
	 * Alp Lab AB: always reconfigure, even for an unchanged data type -- a
	 * new resolution still needs hact/vact, the sensor's LINK_FREQ and the
	 * D-PHY/pixel-clock setup redone below.
	 */
	tmp = fourcc_to_csi_data_type(fmt->pixelformat);
	if (tmp < 0) {
		LOG_ERR("Unsupported CSI pixel format.");
		return tmp;
	}

	for (i = 0; i < ARRAY_SIZE(data_mode_settings); i++) {
		if (data_mode_settings[i].dt == tmp) {
			break;
		}
	}

	data->csi_cpi_settings[data->current_sensor] = &data_mode_settings[i];

	data->time[data->current_sensor].hact = fmt->width;
	data->time[data->current_sensor].vact = fmt->height;

	/*
	 * Alp Lab AB: take the D-PHY lane rate from the sensor's
	 * VIDEO_CID_LINK_FREQ (the v4.4 CSI-2 receiver convention -- upstream
	 * sensors such as imx219 report it there and carry no DT
	 * link-frequencies).  The DT link-frequencies / rx-ddr-clkN value stays
	 * the fallback for a sensor that reports neither LINK_FREQ nor
	 * PIXEL_RATE.
	 */
	link_freq = video_get_csi_link_freq(config->sensor[data->current_sensor],
					    data_mode_settings[i].bits_per_pixel,
					    data->phy[data->current_sensor].num_lanes);
	if (link_freq > 0) {
		data->phy[data->current_sensor].pll_fin = (uint32_t)link_freq;
	}

	return csi2_dw_configure(dev);
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its validation; the sensor-format forwarder loses its `ep` arg.
 */
static int csi2_dw_get_format(const struct device *dev, struct video_format *fmt)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	int ret = -ENODEV;

	if (!fmt) {
		return -EINVAL;
	}

	if (config->sensor[data->current_sensor]) {
		ret = video_get_format(config->sensor[data->current_sensor], fmt);
		if (ret) {
			LOG_ERR("Failed to get sensor format!");
		}
	} else {
		LOG_ERR("Invalid sensor selected!");
	}
	return ret;
}

/*
 * Bench run 76 (stage 5, task "derive int_time_max from the sensor's
 * CURRENT frame interval"): same forwarding shape as csi2_dw_get_format()
 * above, extended to frame interval -- neither this driver nor video_alif.c
 * implemented .get_frmival before now, so isp_pico.c's isp_apply_ae() had
 * no way to learn the sensor's actual configured rate and fell back to a
 * hardcoded 15 fps assumption. Alp Lab AB.
 */
static int csi2_dw_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;

	if (!frmival) {
		return -EINVAL;
	}

	if (!config->sensor[data->current_sensor]) {
		LOG_ERR("Invalid sensor selected!");
		return -ENODEV;
	}

	return video_get_frmival(config->sensor[data->current_sensor], frmival);
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param + its validation; the caps forwarder loses its `ep` arg.
 */
static int csi2_dw_get_caps(const struct device *dev, struct video_caps *caps)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;

	/*
	 * Get the pipeline capabilities from sensor and
	 * send the same data to user.
	 */
	return video_get_caps(config->sensor[data->current_sensor], caps);
}

/*
 * v4.4 video-API shim (Alp Lab AB): the fork's value-pointer ctrl API
 * (set_ctrl/get_ctrl taking `unsigned int cid, void *value`) is gone.  The CSI
 * bridge handled two PRIVATE CIDs by reading the caller's `void *value`:
 *   - VIDEO_CID_ALIF_CSI_DPHY_FREQ -- latch a new D-PHY PLL input frequency;
 *   - VIDEO_CID_ALIF_CSI_CURR_CAM  -- select the active sensor (re-points the
 *     RX D-PHY slave + flushes the IPI FIFO),
 * and otherwise forwarded the CID/value to the active sensor device.  v4.4
 * routes control values through the framework's per-device control registry
 * (video_init_ctrl + struct video_control), so these PRIVATE-base CIDs would
 * need to be registered as device controls before the value-less
 * .set_ctrl(dev, cid) / .get_volatile_ctrl(dev, cid) callbacks could read them
 * back -- a behavioural change beyond a mechanical API shim.  Until those
 * controls are registered (and the sensor-select side-effects re-homed onto the
 * registry path), the forwarding ctrl callbacks are dropped from the API table
 * below; the multi-camera D-PHY-freq / camera-select side-effects are
 * BENCH-UNVERIFIED and deferred.  The single-sensor capture path (the ARX3A0 on
 * this batch) does not exercise either private CID.
 */

static DEVICE_API(video, csi2_dw_driver_api) = {
	.set_format = csi2_dw_set_format,
	.get_format = csi2_dw_get_format,
	.get_frmival = csi2_dw_get_frmival,
	.set_stream = csi2_dw_set_stream,
	.get_caps = csi2_dw_get_caps,
};

static int csi_enable_clocks(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;

	/*
	 * Enable CSI peripheral clock.  The pixel clock is enabled by
	 * csi2_dw_validate_data() once its divisor is set.
	 */
	return clock_control_on(config->clk_dev, config->csiclk);
}

static uint32_t valid_sensor_map(const struct device *const *sensors, int num_sensors)
{
	uint32_t map = 0;

	for (int i = 0; i < num_sensors; i++) {
		if (sensors[i]) {
			map |= BIT(i);
		}
	}

	return map;
}

static uint32_t valid_phy_map(const uint8_t rx_dphy_ids[], int num_dphys)
{
	uint32_t map = 0;

	for (int i = 0; i < num_dphys; i++) {
		map |= BIT(rx_dphy_ids[i]);
	}

	return map;
}

static int csi2_dw_init(const struct device *dev)
{
	const struct csi2_dw_config *config = dev->config;
	struct csi2_dw_data *data = dev->data;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	if (!config->num_dphys) {
		LOG_ERR("Provide at least one dphy");
		return -EINVAL;
	}

	ret = csi_enable_clocks(dev);
	if (ret) {
		LOG_ERR("CAM clock enable failed! Exiting! ret - %d", ret);
		return ret;
	}

	csi2_dw_irq_off(DEVICE_MMIO_GET(dev));
	config->irq_config_func(dev);

	data->current_sensor = config->rx_dphy_ids[0];
	LOG_INF("#rx_dphy_ids: %d", config->num_dphys);

	/*
	 * There should be a one-to-one correspondence between the sensors
	 * available and the D-PHYs available.
	 */
	data->sensors_map = valid_sensor_map(config->sensor, CSI2_NUM_SENSORS) &
		valid_phy_map(config->rx_dphy_ids, config->num_dphys);

	if (!data->sensors_map) {
		LOG_ERR("Incorrect Sensor and DPHY are enabled from DTS");
		return -ENODEV;
	}

	LOG_DBG("MMIO Address: 0x%08x", (uint32_t)DEVICE_MMIO_GET(dev));

	return 0;
}

#define CSI_GET_CLK(i)                                                            \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(i, clocks),                              \
		(.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(i)),                \
		 .pixclk = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(i, \
			 pix_clk, clkid),                                         \
		 .csiclk = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(i, \
			 csi_clk, clkid),))

#define REMOTE_DEVICE(n, id) \
	DT_NODE_REMOTE_DEVICE(DT_INST_ENDPOINT_BY_ID(n, id, 0))

#define REMOTE_EP(n, pid, epid) \
	DT_NODELABEL(DT_STRING_TOKEN(DT_INST_ENDPOINT_BY_ID(n, pid, epid), remote_endpoint_label))

#define CSI2_GET_SENSOR(n, idx) \
	COND_CODE_1(DT_NODE_EXISTS(REMOTE_DEVICE(n, idx)),\
		    (DEVICE_DT_GET_OR_NULL(REMOTE_DEVICE(n, idx))),\
		    (NULL))

#define HANDLE_DPHY_ID_EACH(node_id, prop, idx) \
	DT_PHA_BY_IDX(node_id, prop, idx, id)

#define ALIF_MIPI_CSI_DEVICE(i)                                                                    \
	static void csi2_dw_config_func_##i(const struct device *dev);                             \
	static const struct csi2_dw_config config_##i = {                                          \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(i)),                                              \
		CSI_GET_CLK(i)                                                                     \
		.rx_dphy = DEVICE_DT_GET(DT_INST_PHANDLE(i, phy_if)),                              \
		.rx_dphy_ids = { DT_INST_FOREACH_PROP_ELEM_SEP(i, phy_if,                          \
				HANDLE_DPHY_ID_EACH, (,)) },                                       \
		.num_dphys = DT_INST_PROP_LEN(i, phy_if),                                          \
		.sensor[0] = CSI2_GET_SENSOR(i, 0),                                                \
		.sensor[1] = CSI2_GET_SENSOR(i, 1),                                                \
                                                                                                   \
		.irq = DT_INST_IRQN(i),                                                            \
		.irq_config_func = csi2_dw_config_func_##i,                                        \
                                                                                                   \
		.ipi_mode = DT_INST_ENUM_IDX(i, ipi_mode),                                         \
	};                                                                                         \
                                                                                                   \
	static struct csi2_dw_data data_##i = {                                                    \
                                                                                                   \
		.streaming_map = 0,                                                                \
		.sensors_map = 0,                                                                  \
		.phy[0] =  {                                                                       \
			.num_lanes = COND_CODE_1(DT_NODE_HAS_PROP(REMOTE_EP(i, 0, 0),              \
						data_lanes),                                       \
					(DT_PROP_LEN(REMOTE_EP(i, 0, 0), data_lanes)),             \
					(DT_INST_PROP(i, data_lanes1))),                           \
			.pll_fin = COND_CODE_1(DT_NODE_HAS_PROP(REMOTE_EP(i, 0, 0),                \
						link_frequencies),                                 \
					(DT_PROP_LAST(REMOTE_EP(i, 0, 0), link_frequencies)),      \
					(DT_INST_PROP(i, rx_ddr_clk1))),                           \
			/* issue #2287: see dphy_dw.h's skip_clk_lane_stopstate comment */         \
			.skip_clk_lane_stopstate = DT_PROP_OR(REMOTE_EP(i, 0, 0),                 \
					no_lp11_clock_lane_park, 0),                               \
		},                                                                                 \
                                                                                                   \
		.phy[1] =  {                                                                       \
			.num_lanes = COND_CODE_1(DT_NODE_HAS_PROP(REMOTE_EP(i, 1, 0),              \
						data_lanes),                                       \
					(DT_PROP_LEN(REMOTE_EP(i, 1, 0), data_lanes)),             \
					(DT_INST_PROP(i, data_lanes1))),                           \
			.pll_fin = COND_CODE_1(DT_NODE_HAS_PROP(REMOTE_EP(i, 1, 0),                \
						link_frequencies),                                 \
					(DT_PROP_LAST(REMOTE_EP(i, 1, 0), link_frequencies)),      \
					(DT_INST_PROP(i, rx_ddr_clk2))),                           \
			/* issue #2287: see dphy_dw.h's skip_clk_lane_stopstate comment */         \
			.skip_clk_lane_stopstate = DT_PROP_OR(REMOTE_EP(i, 1, 0),                 \
					no_lp11_clock_lane_park, 0),                               \
		},                                                                                 \
                                                                                                   \
		.time[0] = {                                                                       \
			.hsa = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hsa),                      \
					   (DT_INST_PROP(i, csi_hsa)),                             \
					   (3)),                                                   \
			.hbp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hbp),                      \
					   (DT_INST_PROP(i, csi_hbp)),                             \
					   (0)),                                                   \
			.hsd = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hsd),                      \
					   (DT_INST_PROP(i, csi_hsd)),                             \
					   (0)),                                                   \
			.hact = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hact),                    \
					    (DT_INST_PROP(i, csi_hact)),                           \
					    (0)),                                                  \
			.vsa = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vsa),                      \
					   (DT_INST_PROP(i, csi_vsa)),                             \
					   (0)),                                                   \
			.vbp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vbp),                      \
					   (DT_INST_PROP(i, csi_vbp)),                             \
					   (0)),                                                   \
			.vfp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vfp),                      \
					   (DT_INST_PROP(i, csi_vfp)),                             \
					   (0)),                                                   \
			.vact = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vact),                    \
					    (DT_INST_PROP(i, csi_vact)),                           \
					    (0)),                                                  \
		},                                                                                 \
		.time[1] = {                                                                       \
			.hsa = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hsa),                      \
					   (DT_INST_PROP(i, csi_hsa)),                             \
					   (3)),                                                   \
			.hbp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hbp),                      \
					   (DT_INST_PROP(i, csi_hbp)),                             \
					   (0)),                                                   \
			.hsd = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hsd),                      \
					   (DT_INST_PROP(i, csi_hsd)),                             \
					   (0)),                                                   \
			.hact = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_hact),                    \
					    (DT_INST_PROP(i, csi_hact)),                           \
					    (0)),                                                  \
			.vsa = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vsa),                      \
					   (DT_INST_PROP(i, csi_vsa)),                             \
					   (0)),                                                   \
			.vbp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vbp),                      \
					   (DT_INST_PROP(i, csi_vbp)),                             \
					   (0)),                                                   \
			.vfp = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vfp),                      \
					   (DT_INST_PROP(i, csi_vfp)),                             \
					   (0)),                                                   \
			.vact = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, csi_vact),                    \
					    (DT_INST_PROP(i, csi_vact)),                           \
					    (0)),                                                  \
		},                                                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(i, &csi2_dw_init, NULL, &data_##i, &config_##i, POST_KERNEL,         \
			      CONFIG_VIDEO_MIPI_CSI2_DW_INIT_PRIORITY, &csi2_dw_driver_api);       \
                                                                                                   \
	/* Chains this device onto v4.4's control-registry walk (video_find_ctrl(),               \
	 * drivers/video/video_ctrls.c): a control request against the CSI bridge                 \
	 * falls through to .src_dev, sensor[0] -- the primary of the two muxed                    \
	 * sensor ports this instance can host (config_##i.sensor[]).  Dual-sensor                 \
	 * boards get correct resolution only for whichever sensor is on port 0;                   \
	 * a src_dev that tracks the ACTIVE mux leg is future work, not needed by                  \
	 * any board this SDK ships today (single-sensor per CSI instance). */                     \
	VIDEO_DEVICE_DEFINE(csi_vdev_##i, DEVICE_DT_INST_GET(i), CSI2_GET_SENSOR(i, 0));           \
                                                                                                   \
	static void csi2_dw_config_func_##i(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(i), DT_INST_IRQ(i, priority), csi2_dw_irq,                \
			    DEVICE_DT_INST_GET(i), 0);                                             \
		irq_enable(DT_INST_IRQN(i));                                                       \
	}

DT_INST_FOREACH_STATUS_OKAY(ALIF_MIPI_CSI_DEVICE)
