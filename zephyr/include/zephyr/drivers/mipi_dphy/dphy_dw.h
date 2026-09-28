/*
 * Copyright (c) 2026 Alp Lab AB (provenance header).
 * Vendored VERBATIM from the Apache-2.0 zephyr_alif fork, EXCEPT for one
 * locally-added field on `struct dphy_csi2_settings`
 * (`skip_clk_lane_stopstate`, see its own comment below) -- see
 * zephyr/drivers/mipi_dphy/dphy_dw.c's "local changes" note (issue #2287).
 * ADR 0017 Tier-2 (INTERIM).
 */

#ifndef __ZEPHYR_INCLUDE_DRIVERS_DPHY_DW_H__
#define __ZEPHYR_INCLUDE_DRIVERS_DPHY_DW_H__

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <stddef.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Utility functions. */
#define CEIL(x) (((x) - (uint32_t)(x)) > 0 ? ((uint32_t)(x) + 1) : (uint32_t)(x))
#define ROUND(x) (uint32_t)((double)(x) + 0.5)

struct dphy_dsi_settings {
	/* Number of lanes in D-PHY. */
	uint8_t num_lanes;
	/* Output PLL frequency. */
	uint32_t pll_fout;
	/* Clk Lane HS->LP and LP->HS timings. */
	uint16_t clk_hs2lp;
	uint16_t clk_lp2hs;
	/* Data Lane HS->LP and LP->HS timings. */
	uint16_t lane_hs2lp;
	uint16_t lane_lp2hs;

	uint16_t pll_m;
	uint16_t pll_n;
	uint8_t pll_p;
	uint8_t vco_cntrl;
};

struct dphy_csi2_settings {
	/* Number of lanes in D-PHY. */
	uint8_t num_lanes;
	/* Input PLL frequency. */
	uint32_t pll_fin;
	/*
	 * LOCAL ADDITION (not in the upstream fork), issue #2287: some
	 * continuous-clock sensors (e.g. IMX296) never present a Stop-state
	 * (LP-11) on their CSI-2 CLOCK lane -- unlike OV5647, which the driver
	 * actively parks into LP-11 before this wait runs (see
	 * zephyr/drivers/video/ov5647.c's ov5647_lane_park()). Set from the
	 * sensor's own DT endpoint (`no-lp11-clock-lane-park`,
	 * zephyr/dts/bindings/video/sony,imx296.yaml) when there is no
	 * sensor-side register known to force LP-11 on that lane. When true,
	 * dphy_dw_slave_setup() skips ONLY the clock-lane Stop-state bit
	 * (CSI_PHY_STOPSTATE_PHY_STOPSTATECLK) in its bounded wait -- every
	 * data lane's Stop-state bit, and the wait's timeout, stay fatal for
	 * every sensor, including this one. Never set for a sensor whose
	 * driver already guarantees LP-11 (OV5647, OV9281).
	 */
	bool skip_clk_lane_stopstate;
};

/*
 * Setup the D-PHY as TX-PHY.
 */
int dphy_dw_master_setup(const struct device *dev,
		struct dphy_dsi_settings *phy);

/*
 * Setup the D-PHY as RX-PHY.
 */
int dphy_dw_slave_setup(const struct device *dev,
		struct dphy_csi2_settings *phy,
		uint8_t dphy_id);

int dphy_dw_slave_select(const struct device *dev,
		uint8_t slave_id);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __ZEPHYR_INCLUDE_DRIVERS_DPHY_DW_H__ */
