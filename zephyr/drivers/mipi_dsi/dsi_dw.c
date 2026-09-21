/*
 * Copyright (C) 2024 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM) ======
 * Synopsys DesignWare MIPI-DSI host controller (compatible
 * "snps,designware-dsi"): the DSI-TX bridge that takes the CDC200 DPI stream and
 * drives the shared DesignWare MIPI D-PHY (TX role) on the Alif Ensemble E8.
 * Upstream Zephyr v4.4 ships NO snps,designware-dsi host driver and hal_alif
 * ships no DSI class driver, so the whole driver is carried in-tree so it
 * survives a `west update` (the MIPI_DPHY_DW / VIDEO_*_ALIF precedents).  The
 * D-PHY TX role it depends on is already covered by the in-tree
 * zephyr/drivers/mipi_dphy/dphy_dw.c (dphy_dw_master_setup).
 * Retire onto the opt-in sdk-alif fork once the dsi node is repointed AND
 * bench-verified (task #21).
 * ==================================================================
 *
 * Vendored from the Apache-2.0 zephyr_alif fork (drivers/mipi_dsi/dsi_dw.c) and
 * PORTED to the v4.4 MIPI-DSI host API by Alp Lab AB.  The v4.4 mipi_dsi class
 * API (struct mipi_dsi_driver_api {.attach,.transfer,.detach}, struct
 * mipi_dsi_device, struct mipi_dsi_msg) matches the fork's usage, so the port is
 * mechanical: the fork included a public <zephyr/drivers/mipi_dsi/dsi_dw.h>
 * that never existed in the fork; alp-sdk authors it (zephyr/include/) with the
 * `enum dsi_dw_mode` + dsi_dw_set_mode() the fork expected from it.
 * DEVICE_API() is the v4.4 spelling of the driver-api instance.
 * ALP-SDK PORT FIX: attach, transfer and set_mode hold a per-host k_mutex, so a
 * panel driver's DCS traffic cannot interleave with a display blanking mode
 * switch on the host registers.  vendor-ext, BENCH-UNVERIFIED.
 */
#define DT_DRV_COMPAT snps_designware_dsi

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/device_mmio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/mipi_dphy/dphy_dw.h>
#include "dsi_dw.h"

LOG_MODULE_REGISTER(dsi_dw, CONFIG_MIPI_DSI_LOG_LEVEL);

/* Utility functions. */
static int dsi_format_to_bpp(uint8_t color_coding)
{
	switch (color_coding) {
	case MIPI_DSI_PIXFMT_RGB565:
		return 16;
	case MIPI_DSI_PIXFMT_RGB666:
	case MIPI_DSI_PIXFMT_RGB888:
		return 24;
	case MIPI_DSI_PIXFMT_RGB666_PACKED:
		return 18;
	}
	return -EINVAL;
}

static void reg_write_part(uintptr_t reg, uint32_t data,
		uint32_t mask, uint8_t shift)
{
	uint32_t tmp = 0;

	tmp = sys_read32(reg);
	tmp &= ~(mask << shift);
	tmp |= (data & mask) << shift;
	sys_write32(tmp, reg);
}

/* Helper functions */
void dsi_dw_pwr_down(uintptr_t regs)
{
	sys_clear_bits(regs + DSI_PWR_UP, DSI_PWR_UP_SHUTDOWNZ);
}

void dsi_dw_pwr_up(uintptr_t regs)
{
	sys_set_bits(regs + DSI_PWR_UP, DSI_PWR_UP_SHUTDOWNZ);
}

/*
 * ALP-SDK PORT FIX: the deferred power-up.  dsi_dw_attach_locked() configures
 * the host but deliberately leaves DSI_PWR_UP[SHUTDOWNZ] = 0, so the panel's
 * RESX can rise while the D-PHY sits in LP-11 (see the comment at the end of
 * attach).  The host is powered here instead, at the first operation that
 * actually needs it -- a transfer or a mode switch, both of which run after the
 * panel driver's reset pulse.  Idempotent through data->powered, which also
 * keeps the flag true across the power-cycle bracket in
 * dsi_dw_set_mode_locked(), so the flag never claims a reset host is running
 * (or the reverse).
 */
static void dsi_dw_pwr_up_once(const struct device *dev)
{
	struct dsi_dw_data *data = dev->data;

	if (data->powered)
		return;

	dsi_dw_pwr_up(DEVICE_MMIO_GET(dev));
	data->powered = true;
}

void dsi_dw_wait_2_frames(uint32_t pixclk,
		const struct mipi_dsi_timings *timings)
{
	uint32_t htotal = timings->hactive + timings->hfp + timings->hbp +
		timings->hsync;
	uint32_t vtotal = timings->vactive + timings->vfp + timings->vbp +
		timings->vsync;

	uint32_t den = htotal * vtotal;
	uint32_t tmp = DIV_ROUND_UP(pixclk, den) << 1;

	k_sleep(K_MSEC(tmp));
}

void dsi_dw_intr_en(uintptr_t regs)
{
	sys_read32(regs + DSI_INT_ST0);
	sys_read32(regs + DSI_INT_ST1);

	sys_set_bits(regs + DSI_INT_MSK0, DSI_INT_0_DPHY_ERR_4 |
					DSI_INT_0_DPHY_ERR_3 |
					DSI_INT_0_DPHY_ERR_2 |
					DSI_INT_0_DPHY_ERR_1 |
					DSI_INT_0_DPHY_ERR_0 |
					DSI_INT_0_ACK_WITH_ERR_15 |
					DSI_INT_0_ACK_WITH_ERR_14 |
					DSI_INT_0_ACK_WITH_ERR_13 |
					DSI_INT_0_ACK_WITH_ERR_12 |
					DSI_INT_0_ACK_WITH_ERR_11 |
					DSI_INT_0_ACK_WITH_ERR_10 |
					DSI_INT_0_ACK_WITH_ERR_9 |
					DSI_INT_0_ACK_WITH_ERR_8 |
					DSI_INT_0_ACK_WITH_ERR_7 |
					DSI_INT_0_ACK_WITH_ERR_6 |
					DSI_INT_0_ACK_WITH_ERR_5 |
					DSI_INT_0_ACK_WITH_ERR_4 |
					DSI_INT_0_ACK_WITH_ERR_3 |
					DSI_INT_0_ACK_WITH_ERR_2 |
					DSI_INT_0_ACK_WITH_ERR_1 |
					DSI_INT_0_ACK_WITH_ERR_0);

	sys_set_bits(regs + DSI_INT_MSK1, DSI_INT_1_DPI_BUFF_PLD_UNDER |
					DSI_INT_1_GEN_PLD_RECEV_ERR |
					DSI_INT_1_GEN_PLD_SEND_ERR |
					DSI_INT_1_GEN_PLD_WR_ERR |
					DSI_INT_1_GEN_CMD_WR_ERR |
					DSI_INT_1_DPI_PLD_WR_ERR |
					DSI_INT_1_EOTP_ERR |
					DSI_INT_1_PKT_SIZE_ERR |
					DSI_INT_1_CRC_ERR |
					DSI_INT_1_ECC_MULTI_ERR |
					DSI_INT_1_ECC_SINGLE_ERR |
					DSI_INT_1_TO_LP_RX |
					DSI_INT_1_TO_HP_TX);
}

/* Setup functions */
void dsi_dw_phy_clk_timer_setup(uintptr_t regs,
		struct dphy_dsi_settings *phy)
{
	reg_write_part(regs + DSI_PHY_TMR_LPCLK_CFG,
			phy->clk_hs2lp,
			DSI_PHY_TMR_LPCLK_CFG_HS2LP_MASK,
			DSI_PHY_TMR_LPCLK_CFG_HS2LP_SHIFT);

	reg_write_part(regs + DSI_PHY_TMR_LPCLK_CFG,
			phy->clk_lp2hs,
			DSI_PHY_TMR_LPCLK_CFG_LP2HS_MASK,
			DSI_PHY_TMR_LPCLK_CFG_LP2HS_SHIFT);
}

void dsi_dw_phy_data_timer_setup(uintptr_t regs,
		struct dphy_dsi_settings *phy)
{
	reg_write_part(regs + DSI_PHY_TMR_CFG,
			phy->lane_hs2lp,
			DSI_PHY_TMR_CFG_HS2LP_MASK,
			DSI_PHY_TMR_CFG_HS2LP_SHIFT);

	reg_write_part(regs + DSI_PHY_TMR_CFG,
			phy->lane_lp2hs,
			DSI_PHY_TMR_CFG_LP2HS_MASK,
			DSI_PHY_TMR_CFG_LP2HS_SHIFT);
}

void dsi_dw_setup_phy_timings(const struct device *dev)
{
	struct dsi_dw_data *data = dev->data;
	struct dphy_dsi_settings *phy = &data->phy;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/* Setup D-PHY Clk and Data lane configuration. */
	dsi_dw_phy_clk_timer_setup(regs, phy);
	dsi_dw_phy_data_timer_setup(regs, phy);
}

int dsi_dw_phy_config(const struct device *dev,
		const struct mipi_dsi_device *mdev)
{
	const struct dsi_dw_config *config = dev->config;
	struct dsi_dw_data *data = dev->data;
	struct dphy_dsi_settings *phy = &data->phy;
	int ret;

	/* Do the D-PHY configuration here. */
	ret = dphy_dw_master_setup(config->tx_dphy, phy);
	if (ret) {
		LOG_ERR("Failed to set-up D-PHY TX");
		return ret;
	}

	dsi_dw_setup_phy_timings(dev);

	return 0;
}

int dw_calc_clocks(const struct device *dev,
	const struct mipi_dsi_device *mdev)
{
	const struct mipi_dsi_timings *timings = &mdev->timings;
	struct dsi_dw_data *data = dev->data;
	struct dphy_dsi_settings *phy = &data->phy;
	const struct dsi_dw_config *config = dev->config;

	uint32_t dpi_pix_clk;
	uint8_t esc_clk_div;
	uint32_t htotal;
	uint32_t vtotal;
	float hs_bit_clk;
	int ret = 0;

	/*
	 * ALP-SDK PORT FIX: derive the DPI pixel clock from the panel timings, NOT
	 * from clock_control_get_rate(pix_cid).  The upstream Alif clockctrl's
	 * get_rate returns the pixel clock's PARENT rate (SYST_ACLK, 400 MHz), not
	 * the post-divider pixel rate -- feeding 400 MHz here drives the D-PHY HS
	 * target to ~3.8 GHz and PHY config fails, so dsi attach (and the panel
	 * init) never completes.  The rate comes from the cdc-if controller's
	 * clock-frequency, the rate the CDC is actually run at; without it,
	 * htotal*vtotal*60 Hz.  It MUST match the real CDC rate: in non-burst
	 * mode a faster feed overflows the DPI payload FIFO every line
	 * (INT_ST1 DPI_PLD_WR_ERR).  (The clocks are still wired for the gate
	 * enable in dsi_dw_enable_clocks(); only the RATE source changed.)
	 */
	htotal = timings->hsync + timings->hbp + timings->hactive + timings->hfp;
	vtotal = timings->vsync + timings->vbp + timings->vactive + timings->vfp;
	dpi_pix_clk = config->dpi_pix_clk ? config->dpi_pix_clk
					  : htotal * vtotal * DPI_FRAME_RATE;

	if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
		LOG_DBG("Burst mode of clock calculation");
		/*
		 * We get 1 pixel in 1 pixel-clock cycle. Each pixel can be
		 * made up of 24/16/18 - bits, based on the encoding used. For
		 * bandwidth considerations, the DSI in HS mode should be able
		 * to support similar bandwidth as done by the DPI interface.
		 */
		hs_bit_clk = (dpi_pix_clk *
				dsi_format_to_bpp(mdev->pixfmt));
		hs_bit_clk /= mdev->data_lanes;

		/*
		 * We can run the PLL at 20% higher of the desired Bandwidth
		 * as per the above calculations.
		 */
		hs_bit_clk *= DSI_HS_CLK_SCALING_FACTOR;
		LOG_DBG("hs_bit_clk: %f", (double)hs_bit_clk);
	} else {
		float tmp;

		LOG_DBG("Non-Burst mode of clock calculation");

		hs_bit_clk = ((data->pkt_size *
			      dsi_format_to_bpp(mdev->pixfmt)) / 8.0) + 12;
		hs_bit_clk = (hs_bit_clk / data->pkt_size) *
				dpi_pix_clk;
		hs_bit_clk *= (8.0f / mdev->data_lanes);

		if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			tmp = (64.0 / mdev->data_lanes) * dpi_pix_clk;
		else
			tmp = (32.0 / mdev->data_lanes) * dpi_pix_clk;
		tmp /= timings->hactive;

		hs_bit_clk += tmp;
	}

	/* Clamp the Datarate to maximum panel supported. */
	if (hs_bit_clk > config->panel_max_lane_bw)
		hs_bit_clk = config->panel_max_lane_bw;

	phy->pll_fout = ((uint32_t) hs_bit_clk) >> 1;
	LOG_DBG("PLL Fout requested - %d", phy->pll_fout);

	/* Setup the PLL frequency. */
	ret = dsi_dw_phy_config(dev, mdev);
	if (ret) {
		LOG_ERR("Phy configuration failed.");
		return ret;
	}

	/*
	 * scale = hs_lane_byte_clk/dpi_pix_clk
	 *	 = hs_bit_clk/(8 * dpi_pix_clk)
	 */
	data->clk_scale = ((double)(phy->pll_fout >> 2)) / dpi_pix_clk;
	data->dpi_pix_clk = dpi_pix_clk;
	data->lane_byte_clk = phy->pll_fout >> 2;

	/* Calculate NULL-packet and Number of chunks size. */
	if (!(mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)) {
		float tmp;

		tmp = data->clk_scale * phy->num_lanes;
		if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			tmp -= (8.0f / timings->hactive);
		else
			tmp -= (4.0f / timings->hactive);
		tmp = tmp * data->pkt_size - ((dsi_format_to_bpp(mdev->pixfmt) *
					data->pkt_size) / 8.0f);
		tmp -= 12;

		if (tmp < 0)
			data->null_size = 0;
		else
			data->null_size = ROUND(tmp);
		data->num_chunks = (timings->hactive / data->pkt_size);
		if (data->null_size == 0 && data->num_chunks == 1) {
			data->num_chunks = 0;
		}
	}

	/*
	 * Generate the TX-Escape Clock. Escape Clk Divider values 0/1 stops
	 * the escape clock generation.
	 */
	if (data->lane_byte_clk < MAX_ESC_CLK) {
		esc_clk_div = 2;
	} else {
		esc_clk_div = (data->lane_byte_clk / MAX_ESC_CLK) + 1;
	}
	data->esc_clk_div = esc_clk_div;

	LOG_DBG("Escape clock divider - %d", esc_clk_div);
	LOG_DBG("pixel clock calculated: %d", data->dpi_pix_clk);
	LOG_DBG("lane byte clock calculated: %d", data->lane_byte_clk);
	LOG_DBG("PLL Fout: %d", phy->pll_fout);
	LOG_DBG("Lane byte clock / pixel clock ratio: %f",
			data->clk_scale);
	LOG_DBG("Escape clk value: %d", data->lane_byte_clk/esc_clk_div);
	return 0;
}

void dw_setup_txesc_clk(const struct device *dev)
{
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	reg_write_part(regs + DSI_CLKMGR_CFG,
			data->esc_clk_div,
			DSI_CLKMGR_CFG_TX_ESC_CLK_DIV_MASK,
			DSI_CLKMGR_CFG_TX_ESC_CLK_DIV_SHIFT);

}

void dw_calc_lpcmd_time(const struct device *dev,
		const struct mipi_dsi_device *mdev)
{
	const struct mipi_dsi_timings *timings = &mdev->timings;
	struct dsi_dw_data *data = dev->data;
	struct dphy_dsi_settings *phy = &data->phy;

	uint32_t max_rd_time;
	int outvact;
	int invact;

	/* Line time in number of Escape Clock cycles. */
	outvact = (timings->hsync + timings->hbp + timings->hactive +
			timings->hfp) * data->clk_scale;
	invact = outvact;

	if ((mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) ||
		!(mdev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)) {
		/* HSS packet Transmission and EoTp if it is enabled. */
		outvact -= (4 + ((mdev->mode_flags & MIPI_DSI_MODE_EOT_PACKET) ? 4 : 0)) /
			mdev->data_lanes;
	} else {
		/*
		 * Time for H-Sync active pulse in number of Escape Clock
		 * cycles.
		 */
		outvact -= (timings->hsync * data->clk_scale);
	}

	/* HS->LP and LP->HS transmit time in Escape Clock cycles. */
	outvact -= (phy->lane_hs2lp + phy->lane_lp2hs);
	outvact /= data->esc_clk_div;

	/* LPDT mode entry and DSI controller implemented delay.  */
	outvact -= (22 + 2);

	/*
	 * [MAX_RD_TIME] * LANEBYTECLK_period < [OUTVACT_LPCMD_TIME] *
	 *					16 * TXCLKESC_period
	 *
	 * That constraint is an UPPER bound, and only for reads issued during
	 * active video: it keeps an LP read inside the blanking LP-command
	 * window.  MAX_RD_TIME also has a LOWER bound the formula ignores --
	 * the time the peripheral actually needs to answer -- and using the
	 * upper bound alone silently sizes the read timeout from the LINE TIME.
	 *
	 * That is a real defect, not a theoretical one.  The same panel, same
	 * driver, differing only in pixel clock:
	 *
	 *   RGB888 @ 40 MHz      MAX_RD_TIME 949 @ 60.0 MHz lane byte clk = 15.8 us
	 *                        -> RDDID rc=3, RDDST rc=4 both answer
	 *   RGB565 @ 57.142857   MAX_RD_TIME 591 @ 57.1 MHz lane byte clk = 10.3 us
	 *                        -> RDDID len=3 and RDDST len=4 both rc=-5,
	 *                           while 1- and 2-byte reads still answer
	 *
	 * A longer response takes longer to shift out over LPDT, so the short
	 * reads fit the 10.3 us budget and the longer ones do not.  This is the
	 * "reads of more than one byte never answer" behaviour tracked in #2199;
	 * it was never a panel quirk, it is this timeout moving with the pixel
	 * clock.
	 *
	 * So floor it at what a read needs.  One LP bit takes one escape clock,
	 * i.e. esc_clk_div lane-byte clocks, and the budget below covers a long
	 * response (4-byte header + payload + 2-byte CRC) plus BTA turnaround
	 * and LPDT entry/exit, with room to spare.  Reads are issued in COMMAND
	 * mode, where there is no video LP window to fit inside at all, so
	 * raising it past the video-derived value costs nothing there; a host
	 * that cannot meet both bounds simply cannot read during active video,
	 * which is already what the hardware does.
	 *
	 * The clamp is also load-bearing: outvact is signed and has had the
	 * LPDT-entry delay subtracted, so on a short line it can go NEGATIVE,
	 * and the old expression assigned that straight into a uint32_t --
	 * underflowing to a huge MAX_RD_TIME rather than a small one.
	 */
	if (outvact > 0) {
		max_rd_time = ((uint32_t)outvact * data->esc_clk_div) - 1U;
	} else {
		max_rd_time = 0U;
	}

	if (max_rd_time < (DSI_DW_RD_RESPONSE_BITS * data->esc_clk_div)) {
		max_rd_time = DSI_DW_RD_RESPONSE_BITS * data->esc_clk_div;
	}
	if (max_rd_time > DSI_PHY_TMR_RD_CFG_MAX_RD_TIME_MASK) {
		max_rd_time = DSI_PHY_TMR_RD_CFG_MAX_RD_TIME_MASK;
	}

	/*
	 * OUTVACT LP-CMD Time is time available in bytes to transmit cmd in
	 * LP mode during VSA, VBP and VPF regions.
	 */
	outvact = outvact >> 4;

	invact -= ((timings->hsync + timings->hbp) * data->clk_scale);
	if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
		float tmp;

		tmp = timings->hactive * dsi_format_to_bpp(mdev->pixfmt);
		tmp /= (mdev->data_lanes << 3);
		invact -= (uint32_t) tmp;
	} else {
		invact -= (timings->hactive * data->clk_scale);
	}

	/* HS->LP and LP->HS transmit time in Escape Clock cycles. */
	invact -= (phy->lane_hs2lp + phy->lane_lp2hs);
	invact /= data->esc_clk_div;

	/* LPDT mode entry and DSI controller implemented delay.  */
	invact -= (22 + 2);

	/*
	 * OUTVACT LP-CMD Time is time available in bytes to transmit cmd in
	 * LP mode during VSA, VBP and VPF regions.
	 */
	invact = invact >> 4;

	data->outvact = (outvact > 0) ? outvact : 0;
	data->invact = (invact > 0) ? invact : 0;
	data->max_rd_time = max_rd_time;
	LOG_DBG("OUTVACT - %d INVACT - %d MAX_RD_TIME - %d",
			data->outvact, data->invact, data->max_rd_time);
}

void dw_setup_timeout(const struct device *dev,
		const struct mipi_dsi_device *mdev)
{
	const struct mipi_dsi_timings *timings = &mdev->timings;
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	uint32_t hstx_to;
	uint32_t to_clk_div;
	uint32_t tmp;

	/* Time in lanebyteclocks to send 1 line + 15% of pixel data */
	hstx_to = (timings->hsync + timings->hbp + timings->hfp +
			timings->hactive) * 1.15 * data->clk_scale;

	if (!(mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)) {
		/*
		 * HS-TX timeout for non-burst mode is dependent on time to
		 * transmit 1 frame data.
		 */
		hstx_to *= timings->vactive;
	}

	/*
	 * ALP-SDK PORT FIX: HSTX_TO_CNT is 16 bits.  A non-burst frame (720x1280: ~1.1M
	 * lanebyteclks) does not fit at TO_CLK_DIV, and the register write
	 * used to TRUNCATE it -- a ~8 ms timeout that fired TO_HS_TX inside
	 * every ~17 ms frame.  Widen the (8-bit) timeout-clock divider until
	 * the count fits; the LPRX/BTA timeouts on the same clock only grow.
	 */
	to_clk_div = MAX(TO_CLK_DIV, DIV_ROUND_UP(hstx_to, DSI_TO_CNT_CFG_HSTX_TO_CNT_MASK));
	to_clk_div = MIN(to_clk_div, DSI_CLKMGR_CFG_TO_CLK_DIV_MASK);
	hstx_to /= to_clk_div;
	if (hstx_to > DSI_TO_CNT_CFG_HSTX_TO_CNT_MASK) {
		LOG_WRN("HS-TX timeout %u exceeds HSTX_TO_CNT, clamped", hstx_to);
		hstx_to = DSI_TO_CNT_CFG_HSTX_TO_CNT_MASK;
	}

	reg_write_part(regs + DSI_CLKMGR_CFG, to_clk_div,
			DSI_CLKMGR_CFG_TO_CLK_DIV_MASK,
			DSI_CLKMGR_CFG_TO_CLK_DIV_SHIFT);

	tmp = sys_read32(regs + DSI_TO_CNT_CFG);
	tmp =	((hstx_to & DSI_TO_CNT_CFG_HSTX_TO_CNT_MASK) <<
			DSI_TO_CNT_CFG_HSTX_TO_CNT_SHIFT) |
		((LPRX_TO_CNT & DSI_TO_CNT_CFG_LPRX_TO_CNT_MASK) <<
			DSI_TO_CNT_CFG_LPRX_TO_CNT_SHIFT);
	sys_write32(tmp, regs + DSI_TO_CNT_CFG);

	/*
	 * TODO: Find the values that need to be programmed for HS/LP RD/WR and
	 *  BTA Time-outs, as these values are dependent on the Peripheral
	 *  response time.
	 */
	sys_write32((BTA_TO_CNT & DSI_BTA_TO_CNT_MASK) << DSI_BTA_TO_CNT_SHIFT,
			regs + DSI_BTA_TO_CNT);
}

void dsi_dw_dpi_color_code(uintptr_t regs,
		uint32_t pixfmt)
{
	switch (pixfmt) {
	case MIPI_DSI_PIXFMT_RGB565:
		reg_write_part(regs + DSI_DPI_COLOR_CODING,
				DPI_COLOR_CODE_16B_CONFIG_2,
				DSI_DPI_COLOR_CODING_CLR_MASK,
				DSI_DPI_COLOR_CODING_CLR_SHIFT);
		break;
	case MIPI_DSI_PIXFMT_RGB666:
		sys_set_bits(regs + DSI_DPI_COLOR_CODING,
				DSI_DPI_COLOR_CODING_LOOSELY_18_EN);
	case MIPI_DSI_PIXFMT_RGB666_PACKED:
		reg_write_part(regs + DSI_DPI_COLOR_CODING,
				DPI_COLOR_CODE_18B_CONFIG_2,
				DSI_DPI_COLOR_CODING_CLR_MASK,
				DSI_DPI_COLOR_CODING_CLR_SHIFT);
		break;
	case MIPI_DSI_PIXFMT_RGB888:
		reg_write_part(regs + DSI_DPI_COLOR_CODING,
				DPI_COLOR_CODE_24B,
				DSI_DPI_COLOR_CODING_CLR_MASK,
				DSI_DPI_COLOR_CODING_CLR_SHIFT);
		break;
	}
}

int dsi_dw_burst_mode_setup(uintptr_t regs,
		const struct device *dev,
		const struct mipi_dsi_device *mdev)
{
	const struct mipi_dsi_timings *timings = &mdev->timings;
	struct dsi_dw_data *data = dev->data;
	uint32_t pkt_size;
	uint32_t num_chunks;
	uint32_t null_size;

	if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
		pkt_size = timings->hactive;
		num_chunks = 0;
		null_size = 0;

		reg_write_part(regs + DSI_VID_MODE_CFG,
				DPI_VID_MODE_BURST_0,
				DSI_VID_MODE_CFG_MODE_TYPE_MASK,
				DSI_VID_MODE_CFG_MODE_TYPE_SHIFT);

	} else {
		pkt_size = data->pkt_size;
		num_chunks = data->num_chunks;
		null_size = data->null_size;

		if (mdev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
			reg_write_part(regs + DSI_VID_MODE_CFG,
					DPI_VID_MODE_NON_BURST_SYNC_PULSES,
					DSI_VID_MODE_CFG_MODE_TYPE_MASK,
					DSI_VID_MODE_CFG_MODE_TYPE_SHIFT);
		} else {
			reg_write_part(regs + DSI_VID_MODE_CFG,
					DPI_VID_MODE_NON_BURST_SYNC_EVENTS,
					DSI_VID_MODE_CFG_MODE_TYPE_MASK,
					DSI_VID_MODE_CFG_MODE_TYPE_SHIFT);
		}
	}

	if ((mdev->pixfmt == MIPI_DSI_PIXFMT_RGB666_PACKED) &&
	   (pkt_size % 4)) {
		LOG_ERR("18-bit Loosely packed pixel format should "
				"have vid_pkt_size multiple of 4.");
		return -EINVAL;
	}

	LOG_DBG("PKT_SIZE:%d NUM_CHUNKS:%d NULL_SIZE:%d", pkt_size, num_chunks, null_size);
	reg_write_part(regs + DSI_VID_PKT_SIZE, pkt_size,
			DSI_VID_PKT_SIZE_MASK,
			DSI_VID_PKT_SIZE_SHIFT);

	reg_write_part(regs + DSI_VID_NUM_CHUNKS, num_chunks,
			DSI_VID_NUM_CHUNKS_MASK,
			DSI_VID_NUM_CHUNKS_SHIFT);

	reg_write_part(regs + DSI_VID_NULL_SIZE, null_size,
			DSI_VID_NULL_SIZE_MASK,
			DSI_VID_NULL_SIZE_SHIFT);

	return 0;
}

void dsi_dw_dpi_frame_ack(uintptr_t regs, uint8_t flag)
{
	uint32_t tmp = sys_read32(regs + DSI_VID_MODE_CFG);

	if (flag) {
		tmp |= DSI_VID_MODE_CFG_FRAME_BTA_ACK_EN;
	} else {
		tmp &= ~DSI_VID_MODE_CFG_FRAME_BTA_ACK_EN;
	}
	sys_write32(tmp, regs + DSI_VID_MODE_CFG);
}

void dsi_dw_setup_lp_cmd(const struct device *dev, uint32_t mode_flags)
{
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	if (mode_flags & MIPI_DSI_MODE_LPM) {
		reg_write_part(regs + DSI_DPI_LP_CMD_TIM,
				data->outvact,
				DSI_DPI_LP_CMD_TIM_OUTVACT_MASK,
				DSI_DPI_LP_CMD_TIM_OUTVACT_SHIFT);

		reg_write_part(regs + DSI_DPI_LP_CMD_TIM,
				data->invact,
				DSI_DPI_LP_CMD_TIM_INVACT_MASK,
				DSI_DPI_LP_CMD_TIM_INVACT_SHIFT);

		reg_write_part(regs + DSI_PHY_TMR_RD_CFG,
				data->max_rd_time,
				DSI_PHY_TMR_RD_CFG_MAX_RD_TIME_MASK,
				DSI_PHY_TMR_RD_CFG_MAX_RD_TIME_SHIFT);

		sys_set_bits(regs + DSI_VID_MODE_CFG,
				DSI_VID_MODE_CFG_LP_CMD_EN);
	} else {
		sys_clear_bits(regs + DSI_VID_MODE_CFG,
				DSI_VID_MODE_CFG_LP_CMD_EN);
	}
}

void dsi_dw_packet_handler_config(const struct device *dev)
{
	const struct dsi_dw_config *config = dev->config;
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t tmp;

	tmp = sys_read32(regs + DSI_PCKHDL_CFG);

	if (data->mode_flags & MIPI_DSI_MODE_EOT_PACKET)
		tmp |= DSI_PCKHDL_CFG_EOTP_TX_EN;
	if (config->eotp_lp_tx)
		tmp |= DSI_PCKHDL_CFG_EOTP_TX_LP_EN;
	if (config->eotp_rx)
		tmp |= DSI_PCKHDL_CFG_EOTP_RX_EN;

	if (config->ecc_recv_en)
		tmp |= DSI_PCKHDL_CFG_ECC_RX_EN;
	if (config->crc_recv_en)
		tmp |= DSI_PCKHDL_CFG_CRC_RX_EN;

	/*
	 * ALP-SDK PORT FIX: BTA_EN is not optional here.  Every DCS *read* needs a
	 * bus turnaround, so a host that leaves it clear reports -EIO for reads
	 * that the panel answered.  It is unrelated to frame-ack-en, which is the
	 * end-of-frame ACK in DSI_VID_MODE_CFG (dsi_dw_dpi_frame_ack()).
	 */
	tmp |= DSI_PCKHDL_CFG_BTA_EN;

	sys_write32(tmp, regs + DSI_PCKHDL_CFG);
}

void dsi_dw_panel_timings_setup(const struct device *dev,
		const struct mipi_dsi_timings *timings)
{
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t tmp = timings->hsync + timings->hbp + timings->hfp +
		timings->hactive;

	/* Setup Horizontal timings. */
	reg_write_part(regs + DSI_VID_HSA_TIME,
			ROUND(timings->hsync * data->clk_scale),
			DSI_VID_HSA_TIME_MASK,
			DSI_VID_HSA_TIME_SHIFT);
	reg_write_part(regs + DSI_VID_HBP_TIME,
			ROUND(timings->hbp * data->clk_scale),
			DSI_VID_HBP_TIME_MASK,
			DSI_VID_HBP_TIME_SHIFT);
	reg_write_part(regs + DSI_VID_HLINE_TIME,
			ROUND(tmp * data->clk_scale),
			DSI_VID_HLINE_TIME_MASK,
			DSI_VID_HLINE_TIME_SHIFT);

	/* Setup Vertical timings. */
	reg_write_part(regs + DSI_VID_VSA_LINES, timings->vsync,
			DSI_VID_VSA_LINES_MASK,
			DSI_VID_VSA_LINES_SHIFT);
	reg_write_part(regs + DSI_VID_VBP_LINES, timings->vbp,
			DSI_VID_VBP_LINES_MASK,
			DSI_VID_VBP_LINES_SHIFT);
	reg_write_part(regs + DSI_VID_VFP_LINES, timings->vfp,
			DSI_VID_VFP_LINES_MASK,
			DSI_VID_VFP_LINES_SHIFT);
	reg_write_part(regs + DSI_VID_VACTIVE_LINES, timings->vactive,
			DSI_VID_VACTIVE_LINES_MASK,
			DSI_VID_VACTIVE_LINES_SHIFT);
}

int dsi_dw_dpi_config(const struct device *dev,
		uint8_t channel,
		const struct mipi_dsi_device *mdev)
{
	const struct dsi_dw_config *config = dev->config;
	const struct dpi_config *dpi = &(config->dpi);
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	if (channel > 4) {
		LOG_ERR("VC-ID can be between 0-3 only.");
		return -EINVAL;
	}
	sys_write32(channel, regs + DSI_DPI_VCID);

	/* Setup the polarity of signals DPI signals. */
	sys_write32(dpi->polarity, regs + DSI_DPI_CFG_POL);

	/* Setup the color-coding. */
	dsi_dw_dpi_color_code(regs, mdev->pixfmt);

	/* Burst mode settings. */
	dsi_dw_burst_mode_setup(regs, dev, mdev);

	/* DPI panel timings setup. */
	dsi_dw_panel_timings_setup(dev, &mdev->timings);

	return 0;
}

/*
 * ALP-SDK PORT FIX: clearing PHY_TXREQUESTCLKHS only REQUESTS that the clock
 * lane leave HS -- the D-PHY needs time to complete the HS->LP transition.
 * Issue an LP transaction before that completes and the BTA for a DCS read
 * never gets a turnaround: DSI_INT_ST0 reads back 0x00000000, no error report,
 * nothing.  Measured on E1M-AEN803 2026W36-0009: reads succeed before video mode with
 * DSI_INT_ST0 = 0x00100000, and fail with -EIO and DSI_INT_ST0 = 0x00000000 on
 * every attempt after video mode, including in command mode.
 *
 * Poll DSI_PHY_STATUS.STOPSTATECLKLANE (bit 2) the same way
 * dphy_dw_master_setup() waits for LP-11 after PHY init.  The host must be
 * powered up for the PHY status to be meaningful, so callers that power-cycle
 * the host around a mode switch must call this AFTER dsi_dw_pwr_up().
 */
static void dsi_dw_wait_clklane_stop(uintptr_t regs)
{
	uint32_t stopclk = DSI_PHY_STATUS_PHY_STOPSTATECLKLANE;
	int i;

	for (i = 0; (i < 1000000) && (sys_read32(regs + DSI_PHY_STATUS) & stopclk) != stopclk;
	     i++) {
		k_busy_wait(1);
	}

	if ((sys_read32(regs + DSI_PHY_STATUS) & stopclk) != stopclk) {
		LOG_WRN("Clock lane did not reach stop state after clearing "
			"TXREQUESTCLKHS. DSI_PHY_STATUS=0x%08x",
			sys_read32(regs + DSI_PHY_STATUS));
	}
}

void dsi_dw_msg_config(uintptr_t regs, uint32_t mode_flags)
{
	const uint32_t lp_cmd_mask = DSI_CMD_MODE_CFG_MAX_RD_PKT_SIZE |
				     DSI_CMD_MODE_CFG_DCS_LW_TX |
				     DSI_CMD_MODE_CFG_DCS_SR_0P_TX |
				     DSI_CMD_MODE_CFG_DCS_SW_1P_TX |
				     DSI_CMD_MODE_CFG_DCS_SW_0P_TX |
				     DSI_CMD_MODE_CFG_GEN_LW_TX |
				     DSI_CMD_MODE_CFG_GEN_SR_2P_TX |
				     DSI_CMD_MODE_CFG_GEN_SR_1P_TX |
				     DSI_CMD_MODE_CFG_GEN_SR_0P_TX |
				     DSI_CMD_MODE_CFG_GEN_SW_2P_TX |
				     DSI_CMD_MODE_CFG_GEN_SW_1P_TX |
				     DSI_CMD_MODE_CFG_GEN_SW_0P_TX;
	uint32_t cmd_mode_cfg = sys_read32(regs + DSI_CMD_MODE_CFG) & ~lp_cmd_mask;
	bool lpm = mode_flags & MIPI_DSI_MODE_LPM;

	if (lpm) {
		cmd_mode_cfg |= lp_cmd_mask;
	}
	sys_write32(cmd_mode_cfg, regs + DSI_CMD_MODE_CFG);

	/*
	 * ALP-SDK PORT FIX: clear TXREQUESTCLKHS for LP commands, but only in
	 * command mode: in video mode the HS clock carries the scanout, and an
	 * LP command (a panel set_orientation after blanking_off) would stop
	 * the video.
	 *
	 * cmd_mode is read from the live DSI_MODE_CFG here, not passed in --
	 * callers that are ABOUT TO switch mode (dsi_dw_cmd_mode_config()) must
	 * write DSI_MODE_CFG before calling this, or this reads the mode being
	 * left rather than the mode being entered.
	 */
	bool cmd_mode = sys_read32(regs + DSI_MODE_CFG) & DSI_MODE_CFG_CMD_MODE;
	bool clkhs_off = lpm && cmd_mode;

	sys_write32((clkhs_off ? 0 : DSI_LPCLK_CTRL_PHY_TXREQUESTCLKHS), regs + DSI_LPCLK_CTRL);

	if (clkhs_off) {
		dsi_dw_wait_clklane_stop(regs);
	}

	if (mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)
		sys_set_bits(regs + DSI_LPCLK_CTRL,
				DSI_LPCLK_CTRL_AUTO_CLKLN_CTRL);
}

void dsi_dw_cmd_mode_config(const struct device *dev)
{
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/*
	 * ALP-SDK PORT FIX: set DSI_MODE_CFG to command mode BEFORE the two
	 * calls below, not after.  dsi_dw_msg_config() reads DSI_MODE_CFG live
	 * to decide whether to clear TXREQUESTCLKHS; writing it afterwards (the
	 * original order) made that read see the mode being left, not the mode
	 * being entered, so a video->command transition kept the HS clock
	 * request asserted instead of clearing it.
	 */
	sys_write32(DSI_MODE_CFG_CMD_MODE, regs + DSI_MODE_CFG);

	/* Enable Transmission of commands in LP mode. */
	dsi_dw_setup_lp_cmd(dev, data->mode_flags);

	dsi_dw_msg_config(regs, data->mode_flags);

	data->curr_mode = DSI_DW_COMMAND_MODE;
}

int dsi_dw_video_mode_config(const struct device *dev)
{
	const struct dsi_dw_config *config = dev->config;
	struct dsi_dw_data *data = dev->data;
	const struct dphy_dsi_settings *phy = &data->phy;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t tmp;

	/*
	 * Setup return to Low-Power capability.
	 *
	 * ALP-SDK PORT FIX: the two HORIZONTAL enables are gated on the blanking
	 * actually being long enough for an HS->LP->HS round trip.  The fork and
	 * Linux both set all six unconditionally and trust the controller to
	 * skip an LP entry that does not fit; on this panel's timing it does not
	 * fit by a wide margin, and video never decodes.
	 *
	 * Measured for the RK055HDMIPI4MA0 at 40 MHz / 2 lanes / 24bpp:
	 *   DSI_VID_HLINE_TIME = 1153 lanebyteclks
	 *   one video line on the wire = 4 hdr + 2160 payload + 2 CRC, plus an
	 *     8-byte null packet, over 2 lanes = 1087 lanebyteclks
	 *   => 66 lanebyteclks of horizontal blanking, split HSA 9 / HBP 36 /
	 *      HFP ~21
	 *   one turnaround = lane_hs2lp + lane_lp2hs = 24 + 54 = 78
	 * i.e. a single round trip costs MORE than the entire horizontal
	 * blanking, and HBP and HFP are each far under it on their own.  The
	 * vertical enables keep whole lines of room and stay on.
	 *
	 * If you re-enable these, re-derive the arithmetic above for the new
	 * timing first -- at the panel-native 62.346 MHz the numbers change.
	 */
	{
		const uint32_t turnaround = (uint32_t)phy->lane_hs2lp +
					    (uint32_t)phy->lane_lp2hs;
		const uint32_t hsa   = sys_read32(regs + DSI_VID_HSA_TIME);
		const uint32_t hbp   = sys_read32(regs + DSI_VID_HBP_TIME);
		const uint32_t hline = sys_read32(regs + DSI_VID_HLINE_TIME);
		uint32_t lp_bits = DSI_VID_MODE_CFG_LP_VACT_EN |
				   DSI_VID_MODE_CFG_LP_VFP_EN |
				   DSI_VID_MODE_CFG_LP_VBP_EN |
				   DSI_VID_MODE_CFG_LP_VSA_EN;

		if (hbp > turnaround) {
			lp_bits |= DSI_VID_MODE_CFG_LP_HBP_EN;
		}

		/*
		 * HFP is what is left of the line after sync, back porch AND the
		 * active payload -- so the payload has to be subtracted too.  A
		 * first cut of this tested (hline - hsa - hbp), which is payload
		 * PLUS front porch: 1153 - 9 - 36 = 1108 against a turnaround of
		 * 78, so the predicate passed, LP_HFP_EN stayed set, and the
		 * bench run was void. The front porch alone is ~21.
		 *
		 * One line on the wire is the video packet (4-byte header +
		 * pkt_size * bpp / 8 payload + 2-byte CRC) plus, when one is
		 * configured, a null packet (4 + null_size + 2), spread over
		 * num_lanes.
		 */
		const uint32_t line_bytes = 6U + (data->pkt_size * data->bpp) / 8U +
					    (data->null_size ? (6U + data->null_size) : 0U);
		const uint32_t payload = (phy->num_lanes != 0U) ?
					 (line_bytes / phy->num_lanes) : line_bytes;
		const uint32_t used = hsa + hbp + payload;
		const uint32_t hfp = (hline > used) ? (hline - used) : 0U;

		if (hfp > turnaround) {
			lp_bits |= DSI_VID_MODE_CFG_LP_HFP_EN;
		}

		LOG_DBG("LP blanking: hsa=%u hbp=%u hfp=%u hline=%u payload=%u "
			"turnaround=%u -> bits 0x%x",
			hsa, hbp, hfp, hline, payload, turnaround, lp_bits);
		sys_set_bits(regs + DSI_VID_MODE_CFG, lp_bits);
	}

	/* Setup request for Peripheral ACK at the end of frame. */
	dsi_dw_dpi_frame_ack(regs, config->frame_ack_en);

	/*
	 * ALP-SDK PORT FIX: arm the video pattern generator HERE, not at the end
	 * of attach.  Attach leaves the host in command mode, and the later
	 * command->video switch in dsi_dw_set_mode_locked() brackets itself with
	 * dsi_dw_pwr_down()/dsi_dw_pwr_up(); VPG_EN does not survive that, so a
	 * pattern armed during attach read back as 0 once video was running and
	 * the generator never ran.  Setting it on the video-mode path puts it
	 * after the power-down and before the power-up, where it sticks.
	 */
	switch (config->dpi.vpg_pattern) {
	case DPI_VID_PATTERN_GEN_VERT_COLORBAR:
		sys_set_bits(regs + DSI_VID_MODE_CFG, DSI_VID_MODE_CFG_VPG_EN);
		break;
	case DPI_VID_PATTERN_GEN_HORIZ_COLORBAR:
		sys_set_bits(regs + DSI_VID_MODE_CFG,
			     DSI_VID_MODE_CFG_VPG_EN | DSI_VID_MODE_CFG_VPG_ORIENTATION);
		break;
	case DPI_VID_PATTERN_GEN_VERT_BER:
		sys_set_bits(regs + DSI_VID_MODE_CFG,
			     DSI_VID_MODE_CFG_VPG_EN | DSI_VID_MODE_CFG_VPG_MODE);
		break;
	case DPI_VID_PATTERN_GEN_NONE:
		sys_clear_bits(regs + DSI_VID_MODE_CFG, DSI_VID_MODE_CFG_VPG_EN);
		break;
	}

	tmp = sys_read32(regs + DSI_LPCLK_CTRL);
	if (data->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)
		tmp |= DSI_LPCLK_CTRL_AUTO_CLKLN_CTRL;
	else
		tmp &= ~DSI_LPCLK_CTRL_AUTO_CLKLN_CTRL;
	tmp |= DSI_LPCLK_CTRL_PHY_TXREQUESTCLKHS;
	sys_write32(tmp, regs + DSI_LPCLK_CTRL);

	/* Setup the DSI as Video mode. */
	sys_write32(DSI_MODE_CFG_VID_MODE, regs + DSI_MODE_CFG);

	data->curr_mode = DSI_DW_VIDEO_MODE;
	return 0;
}

/* API functions */
/* Device Specific APIs. */
static int dsi_dw_set_mode_locked(const struct device *dev,
		enum dsi_dw_mode mode)
{
	struct dsi_dw_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);

	/*
	 * ALP-SDK PORT FIX: an unattached host has no timings or PHY setup, so
	 * "switching" it would report success with no video behind it.
	 */
	if (!data->attached)
		return -ENODEV;

	if (mode == data->curr_mode)
		return 0;

	/*
	 * ALP-SDK PORT FIX: a mode switch may be the first thing that runs after
	 * attach, which leaves the host in reset (see dsi_dw_attach_locked()).
	 * Power it up here so the bracket below is a real power-cycle and so
	 * data->powered matches the state the bracket leaves behind (up).
	 */
	dsi_dw_pwr_up_once(dev);

	dsi_dw_pwr_down(regs);
	if (mode == DSI_DW_VIDEO_MODE) {
		/* Setup the DSI as Video mode. */
		dsi_dw_video_mode_config(dev);
	} else {
		/* Setup the DSI as Command mode. */
		sys_write32(DSI_MODE_CFG_CMD_MODE, regs + DSI_MODE_CFG);
		/* Stop the HS clock video mode requested; transfers re-request it. */
		sys_clear_bits(regs + DSI_LPCLK_CTRL, DSI_LPCLK_CTRL_PHY_TXREQUESTCLKHS);
		data->curr_mode = DSI_DW_COMMAND_MODE;
	}
	dsi_dw_pwr_up(regs);

	/*
	 * ALP-SDK PORT FIX: this is the path display_blanking_on() actually
	 * takes (cdc200_blanking_on() -> dsi_dw_set_mode(COMMAND) -> here), and
	 * it clears PHY_TXREQUESTCLKHS on its own rather than through
	 * dsi_dw_msg_config(), so it needs its own wait for the clock lane to
	 * reach LP-11.  Without it every DCS read after a video->command switch
	 * fails -EIO with DSI_INT_ST0 = 0x00000000.  The wait goes AFTER
	 * dsi_dw_pwr_up() because DSI_PHY_STATUS is only meaningful with the
	 * host powered.
	 */
	if (mode != DSI_DW_VIDEO_MODE) {
		dsi_dw_wait_clklane_stop(regs);
	}
	return 0;
}

int dsi_dw_set_mode(const struct device *dev, enum dsi_dw_mode mode)
{
	struct dsi_dw_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = dsi_dw_set_mode_locked(dev, mode);
	k_mutex_unlock(&data->lock);
	return ret;
}

/* Generic APIs */
static int dsi_dw_attach_locked(const struct device *dev,
		uint8_t channel,
		const struct mipi_dsi_device *mdev)
{
	const struct dsi_dw_config *config = dev->config;
	struct dsi_dw_data *data = dev->data;
	struct dphy_dsi_settings *phy = &data->phy;
	struct mipi_dsi_device eff_mdev = *mdev;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	int ret;

	/*
	 * ALP-SDK PORT FIX: always program the cdc-if controller's timings:
	 * this host packs the DPI stream that controller generates, so no
	 * other timings are valid.
	 * Panel drivers need not fill mdev->timings -- upstream hx8394 leaves
	 * it UNINITIALIZED on its stack, and trusting that garbage (the old
	 * "use ours only when hactive == 0" test) set the D-PHY rate, the DPI
	 * line/frame registers and the LP-command windows from stack noise.
	 */
	eff_mdev.timings = config->timings;

	/*
	 * ALP-SDK PORT FIX: the same problem one level up.  A panel driver is
	 * meant to declare the transmission mode its panel needs, but upstream
	 * himax,hx8394 sets only MIPI_DSI_MODE_VIDEO (display_hx8394.c), so burst
	 * and EoTp -- both of which the RK055HDMIPI4MA0 needs, per NXP's own
	 * shield for the identical module -- never reach this host and the panel
	 * stays dark with a perfectly clean video stream on the link.  The board
	 * knows which panel is fitted, so the board supplies them.
	 */
	eff_mdev.mode_flags |= config->mode_flags_or;
	mdev                 = &eff_mdev;

	LOG_DBG("Attach called for channel: %d "
		"With parameters - htimings(%d, %d, %d, %d)\t"
		"vtimings(%d, %d, %d, %d)",
		channel,
		mdev->timings.hsync, mdev->timings.hbp, mdev->timings.hactive,
		mdev->timings.hfp, mdev->timings.vsync, mdev->timings.vbp,
		mdev->timings.vactive, mdev->timings.vfp);

	switch (mdev->pixfmt) {
	case MIPI_DSI_PIXFMT_RGB565:
		LOG_DBG("DSI Interface Format - RGB565");
		break;
	case MIPI_DSI_PIXFMT_RGB666:
		LOG_DBG("DSI Interface Format - RGB666");
		break;
	case MIPI_DSI_PIXFMT_RGB888:
		LOG_DBG("DSI Interface Format - RGB888");
		break;
	case MIPI_DSI_PIXFMT_RGB666_PACKED:
		LOG_DBG("DSI Interface Format - RGB66_PACKED");
		break;
	default:
		LOG_DBG("Unsupported DSI Interface Format");
		return -EINVAL;
	}

	if (!(mdev->mode_flags & MIPI_DSI_MODE_VIDEO)) {
		LOG_ERR("Only Video Mode Panels are supported.");
		return -EINVAL;
	}

	phy->num_lanes = mdev->data_lanes;
	/* Stash for the LP-blanking arithmetic in dsi_dw_video_mode_config(). */
	data->bpp = (uint8_t)dsi_format_to_bpp(mdev->pixfmt);
	data->mode_flags = mdev->mode_flags;

	LOG_DBG("Number of lanes: %d", data->phy.num_lanes);
	LOG_DBG("DSI mode_flags: 0x%x", data->mode_flags);

	dsi_dw_pwr_down(regs);
	data->powered = false;
	ret = dw_calc_clocks(dev, mdev);
	if (ret)
		return ret;
	dw_calc_lpcmd_time(dev, mdev);
	dw_setup_txesc_clk(dev);
	dw_setup_timeout(dev, mdev);
	ret = dsi_dw_dpi_config(dev, channel, mdev);
	if (ret)
		return ret;
	dsi_dw_packet_handler_config(dev);
	dsi_dw_cmd_mode_config(dev);

	/*
	 * The video pattern generator is armed on the video-mode path
	 * (dsi_dw_video_mode_config()), not here: attach leaves the host in
	 * command mode and the later switch to video power-cycles the host,
	 * which clears VPG_EN.
	 */

	/* DSI must wait for 2 frames time after setup. */
	dsi_dw_wait_2_frames(data->dpi_pix_clk, &mdev->timings);
	dsi_dw_intr_en(regs);

	/*
	 * ALP-SDK PORT FIX: attach ends with the host still in RESET -- do NOT
	 * "tidy" a dsi_dw_pwr_up() back in here.  HX8394-F datasheet Fig 5.28:
	 * "MIPI Data Lane and CLK Lane must set LP11 before HW reset go high"
	 * (Fig 5.30 words it "MIPI Data/CLK lane should be LP-11 state before
	 * RESX rising edge").  Alif's DFP satisfies it by leaving the host
	 * unpowered across the panel reset: Driver_MIPI_DSI.c:182 runs
	 * DSI_DPHY_Initialize() to completion (PHY_LOCK + lane stop-state polls,
	 * i.e. lanes driving LP-11), :188 calls display_panel->ops->Init(), which
	 * releases RESX, and DSI_PWR_UP[SHUTDOWNZ] is first written only later, in
	 * DSI_StartCommandMode() (Driver_CDC200.c:382 -> Driver_MIPI_DSI.c:445
	 * dsi_power_up_disable(), :475 dsi_power_up_enable()).
	 * Zephyr inverts that order: hx8394_init() calls mipi_dsi_attach() FIRST
	 * (drivers/display/display_hx8394.c) and only then pulses RESX, so a
	 * power-up here releases the panel's reset with the host already powered
	 * and driving the link, violating LP-11.
	 * What that actually looks like on E1M-AEN803 2026W36-0009, measured, because the
	 * obvious guess is wrong and cost this bring-up a lot of time: the link
	 * stays perfectly healthy.  DCS reads answer (RDDID = 83 94 0f, RDDST =
	 * 81 73 06 00), short writes take effect, generic long writes of every
	 * size land and drain, the panel reports Sleep-Out + Display-On + 24bpp,
	 * scanout runs with INT_ST0/INT_ST1 both 0x00000000 -- and the glass
	 * never shows content.  Do NOT look for a dead link or a dark panel as
	 * the signature of this bug.
	 * dsi_dw_pwr_up_once() powers the host at the first transfer or mode
	 * switch instead; both run after the reset pulse, and a panel with no
	 * reset-gpio is covered because its first transfer still triggers it.
	 */
	data->attached = true;
	return 0;
}

static int dsi_dw_attach(const struct device *dev,
		uint8_t channel,
		const struct mipi_dsi_device *mdev)
{
	struct dsi_dw_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = dsi_dw_attach_locked(dev, channel, mdev);
	k_mutex_unlock(&data->lock);
	return ret;
}

#define HEADER(channel, type, data0, data1)				\
	((((channel) & DSI_GEN_HDR_VC_MASK) << DSI_GEN_HDR_VC_SHIFT) |	\
	(((type) & DSI_GEN_HDR_DT_MASK) << DSI_GEN_HDR_DT_SHIFT) |	\
	(((data0) & DSI_GEN_HDR_WC_LSBYTE_MASK) <<			\
		DSI_GEN_HDR_WC_LSBYTE_SHIFT) |				\
	(((data1) & DSI_GEN_HDR_WC_MSBYTE_MASK) <<			\
		DSI_GEN_HDR_WC_MSBYTE_SHIFT))

int dsi_dw_read_payload(uintptr_t regs, uint8_t *rx, ssize_t len)
{
	uint32_t tmp;
	int i;
	int j;

	/* Wait up to 20 ms for the read command to complete. */
	for (j = 0; j < 20 && (sys_read32(regs + DSI_CMD_PKT_STATUS) &
			       DSI_CMD_PKT_STATUS_GEN_RD_CMD_BUSY); j++)
		k_busy_wait(1000);

	if (sys_read32(regs + DSI_CMD_PKT_STATUS) &
		DSI_CMD_PKT_STATUS_GEN_RD_CMD_BUSY) {
		/* Timed-out during wait for Read operation to finish. */
		return -ETIMEDOUT;
	}

	/* Read from Read Payload FIFO. */
	for (i = 0; i < len; i += 4) {
		/*
		 * Wait till read response is reflected on
		 * Generic Read Payload FIFO.
		 */
		for (j = 0; j < 20 &&
			(sys_read32(regs + DSI_CMD_PKT_STATUS) &
			 DSI_CMD_PKT_STATUS_GEN_PLD_R_EMPTY); j++)
			k_busy_wait(1000);

		if (j == 20) {
			/* Read Payload FIFO is empty. */
			return -EIO;
		}

		tmp = sys_read32(regs + DSI_GEN_PLD_DATA);
		for (j = 0; j < sizeof(uint32_t) && (i + j < len); j++) {
			rx[i + j] = tmp >> (8 * j);
		}
	}
	return 0;
}

/* Poll until the payload write FIFO has room, as Linux's dw-mipi-dsi does. */
static int dsi_dw_wait_pld_space(uintptr_t regs)
{
	for (int j = 0; j < DSI_DW_PLD_SPACE_POLLS; j++) {
		if (!(sys_read32(regs + DSI_CMD_PKT_STATUS) &
		      DSI_CMD_PKT_STATUS_GEN_PLD_W_FULL)) {
			return 0;
		}
		k_usleep(1);
	}

	return -EMSGSIZE;
}

int dsi_dw_write_payload(uintptr_t regs, uint8_t byte0, const uint8_t *tx,
		ssize_t len)
{
	uint32_t payload_word;
	ssize_t i;
	int ret;
	int n;

	/*
	 * Wait for FIFO space BEFORE each write.  The old code wrote first and
	 * read GEN_PLD_W_FULL afterwards, which is too late to save the word,
	 * and then returned -EMSGSIZE from the middle of a payload -- leaving
	 * words in the FIFO with no header to consume them.  That residue is
	 * invisible to GEN_PLD_W_EMPTY below four words (see
	 * DSI_DW_GEN_PATH_DRAINED) and the next packet's payload is appended
	 * to it, misaligning every long write that follows.
	 *
	 * The casts to uint32_t are load-bearing: tx[i] is a uint8_t promoted
	 * to int, so `tx[i] << 24` overflows a signed int for any byte >= 0x80
	 * and is undefined behaviour.  SETEXTC's own payload (B9h FF 83 94)
	 * hits it.
	 */
	payload_word = (uint32_t)byte0 << DSI_GEN_PLD_DATA_B1_SHIFT;
	for (i = 0; (i < len) && (i < 3); i++) {
		payload_word |= (uint32_t)tx[i] << (8 * (i + 1));
	}

	ret = dsi_dw_wait_pld_space(regs);
	if (ret) {
		return ret;
	}
	sys_write32(payload_word, regs + DSI_GEN_PLD_DATA);

	while (i < len) {
		payload_word = 0;
		for (n = 0; (n < 4) && (i < len); i++, n++) {
			payload_word |= (uint32_t)tx[i] << (8 * n);
		}

		ret = dsi_dw_wait_pld_space(regs);
		if (ret) {
			return ret;
		}
		sys_write32(payload_word, regs + DSI_GEN_PLD_DATA);
	}

	return 0;
}

/*
 * Every stage of the generic path drained -- the packet has actually left.
 *
 * GEN_PLD_W_EMPTY alone is not enough, and on this silicon it is actively
 * misleading: measured on the E8, one word written to GEN_PLD_DATA clears
 * GEN_BUFF_PLD_EMPTY (bit 18) while GEN_PLD_W_EMPTY (bit 2) still reads 1,
 * and bit 2 only clears once four words are pending.  A long write with a
 * one-word payload -- SETEXTC (B9h FF 83 94) is exactly that -- therefore
 * satisfied the old wait on its first poll, so the write reported success
 * without anything having been observed to go out.  A payload left behind
 * that way is invisible to bit 2 and the next packet's payload is appended
 * to it, which misaligns every long write that follows.
 */
#define DSI_DW_GEN_PATH_DRAINED						\
	(DSI_CMD_PKT_STATUS_GEN_CMD_EMPTY |				\
	 DSI_CMD_PKT_STATUS_GEN_PLD_W_EMPTY |				\
	 DSI_CMD_PKT_STATUS_GEN_BUFF_CMD_EMPTY |			\
	 DSI_CMD_PKT_STATUS_GEN_BUFF_PLD_EMPTY)

int dsi_dw_write_hdr(uintptr_t regs, uint32_t header)
{
	uint32_t mask = 0;
	int j;
	uint32_t tmp;

	sys_write32(header, regs + DSI_GEN_HDR);

	j = 100;
	mask = DSI_DW_GEN_PATH_DRAINED;
	do {
		tmp = sys_read32(regs + DSI_CMD_PKT_STATUS);
		if ((tmp & mask) == mask)
			break;

		k_usleep(1000);
	} while (j-- > 0);

	if ((mask & sys_read32(regs + DSI_CMD_PKT_STATUS)) != mask) {
		LOG_ERR("Failed to write command FIFO.");
		return -ETIMEDOUT;
	}

	return 0;
}

int dsi_dw_send_max_return_packet_size(uintptr_t regs, uint8_t channel,
		uint16_t value)
{
	uint32_t mask;

	/*
	 * The high byte needs the shift before the narrowing: `(uint8_t) value >> 8`
	 * casts first and is always 0, so a size above 255 was silently truncated.
	 */
	sys_write32(HEADER(channel,
			MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE,
			value & 0xff,
			(value >> 8) & 0xff),
		regs + DSI_GEN_HDR);

	mask = DSI_DW_GEN_PATH_DRAINED;
	for (int j = 0; (j < 100) &&
		(mask & sys_read32(regs + DSI_CMD_PKT_STATUS)) != mask; j++)
		k_usleep(1000);

	if ((mask & sys_read32(regs + DSI_CMD_PKT_STATUS)) != mask) {
		LOG_ERR("Failed to write command FIFO.");
		return -ETIMEDOUT;
	}

	return 0;
}

static ssize_t dsi_dw_transfer_locked(const struct device *dev,
		uint8_t channel,
		struct mipi_dsi_msg *msg)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct dsi_dw_data *data = dev->data;

	const uint8_t *tx = msg->tx_buf;
	uint32_t header;
	uint8_t param0;
	uint8_t param1;
	uint32_t mask;
	uint32_t mode_flags = data->mode_flags;
	int ret;

	if (msg->flags & MIPI_DSI_MSG_USE_LPM)
		mode_flags |= MIPI_DSI_MODE_LPM;
	dsi_dw_setup_lp_cmd(dev, mode_flags);
	dsi_dw_msg_config(regs, mode_flags);

	/*
	 * ALP-SDK PORT FIX: attach leaves the host in reset so the panel's RESX
	 * rises with the lanes in LP-11 (see dsi_dw_attach_locked()), so power it
	 * up here -- before any DSI_CMD_PKT_STATUS poll, DSI_GEN_HDR or
	 * DSI_GEN_PLD_DATA access.  The two config writes above are the
	 * DesignWare-recommended order anyway: program, then power up.
	 */
	dsi_dw_pwr_up_once(dev);

	/* Wait till the Command FIFO has empty space or time-out. */
	mask = DSI_CMD_PKT_STATUS_GEN_CMD_FULL;
	for (int j = 0; (j < 100) &&
		((mask & sys_read32(regs + DSI_CMD_PKT_STATUS)) != 0); j++)
		k_usleep(1000);

	if ((mask & sys_read32(regs + DSI_CMD_PKT_STATUS)) != 0) {
		LOG_ERR("Timed-out waiting get available Command-FIFO.");
		return -ETIMEDOUT;
	}

	switch (msg->type) {
	case MIPI_DSI_DCS_READ:
		/*
		 * Send a maximum return packet size packet to
		 * configure the return response of peripheral.
		 */
		ret = dsi_dw_send_max_return_packet_size(regs,
			channel, msg->rx_len);
		if (ret)
			return ret;
		/* Now write the Read packet. */
		param0 = msg->cmd;
		param1 = 0;
		header = HEADER(channel, msg->type, param0, param1);
		ret = dsi_dw_write_hdr(regs, header);
		if (ret)
			return ret;
		break;
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		param0 = msg->cmd;
		param1 = (msg->tx_len > 0) ? tx[0] : 0;
		header = HEADER(channel, msg->type, param0, param1);
		ret = dsi_dw_write_hdr(regs, header);
		if (ret)
			return ret;
		break;
	case MIPI_DSI_DCS_LONG_WRITE:
		ret = dsi_dw_write_payload(regs, msg->cmd, tx,
				msg->tx_len);
		if (ret)
			return ret;
		param0 = msg->tx_len + 1;
		param1 = (msg->tx_len + 1) >> 8;

		header = HEADER(channel, msg->type, param0, param1);
		ret = dsi_dw_write_hdr(regs, header);
		if (ret)
			return ret;
		break;
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
		/*
		 * Send a maximum return packet size packet to
		 * configure the return response of peripheral.
		 */
		ret = dsi_dw_send_max_return_packet_size(regs,
			channel, msg->rx_len);
		if (ret)
			return ret;
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		param0 = (msg->tx_len > 0) ? tx[0] : 0;
		param1 = (msg->tx_len > 1) ? tx[1] : 0;
		header = HEADER(channel, msg->type, param0, param1);
		ret = dsi_dw_write_hdr(regs, header);
		if (ret)
			return ret;
		break;
	case MIPI_DSI_GENERIC_LONG_WRITE:
		if (msg->tx_len >= 1) {
			ret = dsi_dw_write_payload(regs, tx[0],
					tx + 1, msg->tx_len - 1);
			if (ret)
				return ret;
		}
		param0 = msg->tx_len;
		param1 = msg->tx_len >> 8;
		header = HEADER(channel, msg->type, param0, param1);
		ret = dsi_dw_write_hdr(regs, header);
		if (ret)
			return ret;
		break;
	default:
		LOG_ERR("Un-supported packet type!");
		return -EINVAL;
	}

	if (msg->rx_buf && msg->rx_len) {
		ret = dsi_dw_read_payload(regs, msg->rx_buf, msg->rx_len);
		if (ret < 0)
			return ret;
		/*
		 * The mipi_dsi API asks for the number of bytes received, but the DW
		 * controller does not expose the received word count: the read FIFO is
		 * drained blind.  So a short response is reported as msg->rx_len with
		 * the untouched tail of rx_buf, and a caller cannot tell truncation
		 * from a full answer by the return value alone.
		 */
		return msg->rx_len;
	}

	/*
	 * The MIPI-DSI API contract is to return the number of bytes
	 * transferred, not 0.  Panel drivers rely on this: e.g. display_hx8394
	 * gates its final SET_DISPLAY_ON on `if (ret_tx != 1)`, so returning 0
	 * here makes a fully-successful init report failure (panel never ready).
	 */
	return msg->tx_len;
}

static ssize_t dsi_dw_transfer(const struct device *dev,
		uint8_t channel,
		struct mipi_dsi_msg *msg)
{
	struct dsi_dw_data *data = dev->data;
	ssize_t ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = dsi_dw_transfer_locked(dev, channel, msg);
	k_mutex_unlock(&data->lock);
	return ret;
}

/* ISR Function */
static void dsi_dw_irq(const struct device *dev)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t irq_st0;
	uint32_t irq_st1;
	uint32_t mask;

	irq_st0 = sys_read32(regs + DSI_INT_ST0);
	irq_st1 = sys_read32(regs + DSI_INT_ST1);

	mask = DSI_INT_0_ACK_WITH_ERR_15 | DSI_INT_0_ACK_WITH_ERR_14 |
		DSI_INT_0_ACK_WITH_ERR_13 | DSI_INT_0_ACK_WITH_ERR_12 |
		DSI_INT_0_ACK_WITH_ERR_11 | DSI_INT_0_ACK_WITH_ERR_10 |
		DSI_INT_0_ACK_WITH_ERR_9 | DSI_INT_0_ACK_WITH_ERR_8 |
		DSI_INT_0_ACK_WITH_ERR_7 | DSI_INT_0_ACK_WITH_ERR_6 |
		DSI_INT_0_ACK_WITH_ERR_5 | DSI_INT_0_ACK_WITH_ERR_4 |
		DSI_INT_0_ACK_WITH_ERR_3 | DSI_INT_0_ACK_WITH_ERR_2 |
		DSI_INT_0_ACK_WITH_ERR_1 | DSI_INT_0_ACK_WITH_ERR_0;
	if (irq_st0 & mask)
		LOG_ERR("ACK Error. irq_st0 - 0x%x", irq_st0);

	mask = DSI_INT_0_DPHY_ERR_4 | DSI_INT_0_DPHY_ERR_3 |
		DSI_INT_0_DPHY_ERR_2 | DSI_INT_0_DPHY_ERR_1 |
		DSI_INT_0_DPHY_ERR_0;
	if (irq_st0 & mask)
		LOG_ERR("D-PHY Error. irq_st0 - 0x%x", irq_st0);

	mask = DSI_INT_1_TO_HP_TX | DSI_INT_1_TO_LP_RX |
		DSI_INT_1_ECC_SINGLE_ERR | DSI_INT_1_ECC_MULTI_ERR |
		DSI_INT_1_CRC_ERR | DSI_INT_1_PKT_SIZE_ERR | DSI_INT_1_EOTP_ERR;
	if (irq_st1 & mask)
		LOG_ERR("DSI PKT Error. irq_st1 - 0x%x", irq_st1);

	mask = DSI_INT_1_DPI_PLD_WR_ERR | DSI_INT_1_GEN_CMD_WR_ERR |
		DSI_INT_1_GEN_PLD_WR_ERR | DSI_INT_1_GEN_PLD_SEND_ERR |
		DSI_INT_1_GEN_PLD_RD_ERR | DSI_INT_1_GEN_PLD_RECEV_ERR |
		DSI_INT_1_DPI_BUFF_PLD_UNDER;
	if (irq_st1 & mask)
		LOG_ERR("DSI DPI Error Event. irq_st1 - 0x%x", irq_st1);
}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(clocks)
static int dsi_dw_enable_clocks(const struct device *dev)
{
	const struct dsi_dw_config *config = dev->config;
	int ret;

	/* Enable DSI clock. */
	ret = clock_control_on(config->clk_dev, config->dsi_cid);
	if (ret) {
		LOG_ERR("Enable DSI clock source for APB interface failed! ret - %d", ret);
		return ret;
	}

	return 0;

}
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(clocks) */

static int dsi_dw_init(const struct device *dev)
{
	const struct dsi_dw_config *config = dev->config;
	struct dsi_dw_data *data = dev->data;
	int ret = 0;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	k_mutex_init(&data->lock);

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(clocks)
	ret = dsi_dw_enable_clocks(dev);
	if (ret) {
		LOG_ERR("DSI clock enable failed! Exiting! ret - %d", ret);
		return ret;
	}
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(clocks) */

	config->irq_config_func(dev);

	LOG_DBG("MMIO address: 0x%x", (uint32_t) DEVICE_MMIO_GET(dev));

	LOG_DBG("irq - %d", config->irq);
	LOG_DBG("Video pattern generator: %d", config->dpi.vpg_pattern);
	LOG_DBG("Packet size - %d", data->pkt_size);
	LOG_DBG("Panel Max Lane BW - %d", config->panel_max_lane_bw);
	return 0;
}

static DEVICE_API(mipi_dsi, dsi_dw_api) = {
	.attach = dsi_dw_attach,
	.transfer = dsi_dw_transfer,
};

#define MIPI_DSI_GET_CLK(i)                                                                     \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(i, clocks),                                            \
		(.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(i)),                              \
		 .pix_cid = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(i,              \
			 pixel_clk, clkid),                                                     \
		 .dsi_cid = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(i,              \
			 dsi_clk_en, clkid),))

/*
 * The panel properties the board declares on the host's behalf, because the
 * panel driver does not.  dpi-video-mode's enum order is the binding's:
 * 0 non-burst-sync-events (no flag), 1 non-burst-sync-pulses, 2 burst.
 */
#define DSI_DW_MODE_FLAGS_OR(i)                                                                 \
	((DT_INST_ENUM_IDX(i, dpi_video_mode) == 1 ? MIPI_DSI_MODE_VIDEO_SYNC_PULSE : 0) |      \
	 (DT_INST_ENUM_IDX(i, dpi_video_mode) == 2 ? MIPI_DSI_MODE_VIDEO_BURST : 0) |           \
	 (DT_INST_PROP(i, autoinsert_eotp) ? MIPI_DSI_MODE_EOT_PACKET : 0))

#define ALIF_MIPI_DSI_DEVICE(i)                                                                 \
	static void dsi_dw_config_func_##i(const struct device *dev);				\
	static const struct dsi_dw_config config_##i = {					\
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(i)),						\
												\
		MIPI_DSI_GET_CLK(i)                                                             \
		.tx_dphy = DEVICE_DT_GET(DT_INST_PHANDLE(i, phy_if)),                              \
		.irq = DT_INST_IRQN(i),								\
		.irq_config_func = dsi_dw_config_func_##i,					\
												\
		.dpi = {									\
			.polarity = COND_CODE_0(DT_INST_PROP_BY_PHANDLE(i, cdc_if,		\
							hsync_active),				\
						    DSI_DPI_CFG_POL_HSYNC_ACTIVE_LOW,		\
						    (0)) |					\
				    COND_CODE_0(DT_INST_PROP_BY_PHANDLE(i, cdc_if,		\
							vsync_active),				\
						    DSI_DPI_CFG_POL_VSYNC_ACTIVE_LOW,		\
						    (0)) |					\
				    COND_CODE_0(DT_INST_PROP_BY_PHANDLE(i, cdc_if, de_active),	\
						    DSI_DPI_CFG_POL_DATAEN_ACTIVE_LOW,		\
						    (0)) |					\
				    COND_CODE_0(DT_INST_PROP(i, dpi_colorm_active),		\
						    DSI_DPI_CFG_POL_COLM_ACTIVE_LOW,		\
						    (0)) |					\
				    COND_CODE_0(DT_INST_PROP(i, dpi_shutdn_active),		\
						    DSI_DPI_CFG_POL_SHUTD_ACTIVE_LOW,		\
						    (0)),					\
			.vpg_pattern = DT_INST_ENUM_IDX_OR(i, dpi_video_pattern_gen,		\
						DPI_VID_PATTERN_GEN_NONE),			\
		},										\
		.timings = {									\
			.hactive = DT_INST_PROP_BY_PHANDLE(i, cdc_if, width),		\
			.hfp = DT_INST_PROP_BY_PHANDLE(i, cdc_if, hfront_porch),		\
			.hbp = DT_INST_PROP_BY_PHANDLE(i, cdc_if, hback_porch),		\
			.hsync = DT_INST_PROP_BY_PHANDLE(i, cdc_if, hsync_len),		\
			.vactive = DT_INST_PROP_BY_PHANDLE(i, cdc_if, height),		\
			.vfp = DT_INST_PROP_BY_PHANDLE(i, cdc_if, vfront_porch),		\
			.vbp = DT_INST_PROP_BY_PHANDLE(i, cdc_if, vback_porch),		\
			.vsync = DT_INST_PROP_BY_PHANDLE(i, cdc_if, vsync_len),		\
		},										\
		.eotp_lp_tx = DT_INST_PROP(i, eotp_lp_tx_en),					\
		.eotp_rx = DT_INST_PROP(i, eotp_rx_en),						\
		.ecc_recv_en = DT_INST_PROP(i, ecc_recv_en),					\
		.crc_recv_en = DT_INST_PROP(i, crc_recv_en),					\
		.frame_ack_en = DT_INST_PROP(i, frame_ack_en),					\
		.mode_flags_or = DSI_DW_MODE_FLAGS_OR(i),					\
		.panel_max_lane_bw = DT_INST_PROP(i, panel_max_lane_bandwidth),                 \
		.dpi_pix_clk = DT_PROP_OR(DT_INST_PHANDLE(i, cdc_if), clock_frequency, 0),      \
	};											\
	static struct dsi_dw_data data_##i = {							\
		.pkt_size = COND_CODE_1(DT_INST_NODE_HAS_PROP(i, vid_pkt_size),			\
					(DT_INST_PROP(i, vid_pkt_size)),			\
					(DT_INST_PROP_BY_PHANDLE(i, cdc_if, width))),		\
		.num_chunks = 0,								\
		.null_size = 0,									\
		/* The host leaves reset in command mode (MODE_CFG = 1). */			\
		.curr_mode = DSI_DW_COMMAND_MODE,						\
	};											\
	DEVICE_DT_INST_DEFINE(i,								\
			&dsi_dw_init,								\
			NULL,									\
			&data_##i,								\
			&config_##i,								\
			POST_KERNEL,								\
			CONFIG_MIPI_DSI_INIT_PRIORITY,						\
			&dsi_dw_api);								\
												\
	static void dsi_dw_config_func_##i(const struct device *dev)				\
	{											\
		IRQ_CONNECT(DT_INST_IRQN(i),							\
			DT_INST_IRQ(i, priority),						\
			dsi_dw_irq,								\
			DEVICE_DT_INST_GET(i),							\
			0);									\
		irq_enable(DT_INST_IRQN(i));							\
	}											\

DT_INST_FOREACH_STATUS_OKAY(ALIF_MIPI_DSI_DEVICE)
