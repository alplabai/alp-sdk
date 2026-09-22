/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public, devicetree-independent API of the Synopsys DesignWare MIPI-DSI host
 * driver (zephyr/drivers/mipi_dsi/dsi_dw.c, compatible "snps,designware-dsi").
 * The path mirrors the one the Alif zephyr_alif fork includes; the fork never
 * shipped the header, so it is authored here.  Kept free of DT-dependent
 * types (the driver's private dsi_dw.h sizes its config struct from the
 * devicetree of the including driver) so another driver -- the CDC200
 * display controller -- can call it.  ADR 0017 Tier-2 (INTERIM, task #21).
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MIPI_DSI_DSI_DW_H_
#define ZEPHYR_INCLUDE_DRIVERS_MIPI_DSI_DSI_DW_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Operating mode of a DesignWare MIPI-DSI host. */
enum dsi_dw_mode {
	/** DPI video mode: the host streams its cdc-if controller's pixels. */
	DSI_DW_VIDEO_MODE = 0,
	/** Command mode: panel init and DCS traffic (the state after attach). */
	DSI_DW_COMMAND_MODE = 1,
};

/**
 * @brief Switch a DesignWare MIPI-DSI host between command and video mode.
 *
 * Called by the cdc-if display controller's blanking_off/blanking_on, never
 * before the panel driver's init has attached to the host.  Serialised against
 * mipi_dsi_attach() and mipi_dsi_transfer() on the same host.
 *
 * @param dev  The snps,designware-dsi host device.
 * @param mode Target mode.
 *
 * @retval 0       Switched, or already in @p mode.
 * @retval -ENODEV No panel has attached to the host yet.
 */
int dsi_dw_set_mode(const struct device *dev, enum dsi_dw_mode mode);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MIPI_DSI_DSI_DW_H_ */
