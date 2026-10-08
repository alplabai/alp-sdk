/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR-0017-ADJACENT (register sequence from the Alif DFP / DevKit, not a HAL consumer) ======
 * SoC-level setup the Alif E8 MIPI D-PHY and the display chain on it (CDC200 ->
 * DesignWare DSI -> D-PHY) need and that none of their drivers, nor Zephyr's
 * upstream Alif clock control, performs.  No hal_alif library covers these
 * registers, so each step replays Alif's own documented sequence directly;
 * none reimplements a driver.  Built on E8 when the D-PHY, DSI or CDC200
 * driver is configured (zephyr/CMakeLists.txt), and each hook is gated on its
 * DT node.  See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 *
 * BENCH: 2026-09-18, E1M-AEN803 2026W36-0009, aen-dsi-display Flow C
 * RAM-run.  Register readback after boot: CGU CLK_ENA 0x1a602014 =
 * 0xfeb3fff1 (bits 21 and 23 set), VBAT PWR_CTRL 0x1a609008 = 0x03030000
 * (bits 0/1/4/5/8/9/12 clear), CDC200_PIXCLK_CTRL 0x4903F004 = 0x00070001
 * (divider 7, enabled), CDC L1 framebuffer 0x49031134 = 0x02200000, P5_5 pad
 * 0x1a6030b4 = 0x00010000 (GPIO, REN); backlight read back 1, DSI INT_ST1 =
 * 0x00000000 over 100 ms of scanout.
 * ===================================================================
 *
 * 1. D-PHY clock sources + analog power (PRE_KERNEL_1, dphy okay).  The
 *    MIPI_CKEN gates the D-PHY driver turns on only gate clocks that must
 *    already oscillate upstream, and on an SE-less boot (the Flow C RAM-run)
 *    nothing starts them: the D-PHY PLL then has no 38.4 MHz reference and no
 *    analog supply and never locks (bench: DSI_PHY_STATUS stuck at 0x1400, no
 *    LOCK bit, with the PLL m/n/VCO registers correctly programmed).  So:
 *      - CGU CLK_ENA (+0x14): bit 21 = HFOSC (38.4 MHz, the PLL reference),
 *        bit 23 = the 100 MHz CFG clock (source of the 25 MHz D-PHY config
 *        clock that drives the PHY startup/lock state machine);
 *      - VBAT PWR_CTRL (+0x08): clear the MIPI TX/RX/PLL D-PHY power masks
 *        (bits 0/4/8), their isolation (bits 1/5/9) and the VPH-1P8 bypass
 *        (bit 12); at reset they leave the D-PHY analog islands off and
 *        isolated.
 *    Provenance: the Alif sdk-alif display sample's CGU write and the Alif
 *    DevKit-E8 vbat_init(); no Secure Enclave service involved.  The same
 *    power-up applies to the CSI (RX) role, so camera builds get it too.
 *
 * 2. CDC200 pixel-clock divider (POST_KERNEL, before the display and DSI
 *    drivers; cdc200 okay with a clock-frequency).  Upstream
 *    clock_control_alif has no .set_rate for this clock, so
 *    CDC200_PIXCLK_CTRL[24:16] keeps its reset divisor 511 (~0.78 MHz).  The
 *    divider is set to SYST_ACLK / the cdc200 clock-frequency, which must
 *    divide exactly (BUILD_ASSERT): the DSI host times its lanes from that
 *    same property, and when the two disagreed the DPI payload FIFO
 *    overflowed every line (#2199).  Other bits are kept; the enable (bit 0)
 *    is set later by the cdc200 driver's clock_control_on().  Register:
 *    CLKCTL_PER_MST + 0x04 (AE822 DFP sys_ctrl_cdc.h).
 *
 * 3. DSI panel backlight pad (PRE_KERNEL_1, CONFIG_MIPI_DSI_DW, a mipi_dsi
 *    panel child whose bl-gpios is on a main-domain DesignWare GPIO port,
 *    gpio0..gpio14).
 *    gpio_dw applies no pad mux, and the panel driver only drives bl-gpios,
 *    so the pad is muxed to its GPIO function (alt 0) with the input receiver
 *    on, so the level also reads back.  A bl-gpios on any other controller
 *    (an I2C expander, lpgpio) needs no Alif pad mux from here and is
 *    skipped.  The CDC200 driver's own pinctrl-0 is not applied in a MIPI-DSI
 *    build, which is why this lives here and not in a pinctrl group on
 *    &cdc200.
 *
 * LATENT HAZARD -- the Secure Enclave run profile.  hal_alif's aiPM
 * run_profile_t (se_services/include/aipm.h) also manages these blocks:
 * phy_pwr_gating (MIPI_TX_DPHY / MIPI_RX_DPHY / MIPI_PLL_DPHY) and
 * ip_clock_gating (CDC200, MIPI_DSI).  The writes here bypass the SE, so a
 * SERVICES_set_run_cfg() / se_service_set_run_cfg() whose profile lacks
 * those bits may power the D-PHY down or gate the CDC200/DSI clocks under a
 * running display.  In-tree, only alp_power_profile_set(RUN)
 * (src/backends/power/alif_se_profile.c) calls it, as a read-modify-write
 * of the SE's own profile, which does not know about these register-direct
 * enables.  Not observed; nothing in the display path calls it.
 *
 * Base addresses come from the upstream clock-controller node's reg-names
 * ("cgu", "vbat", "clkctl_per_mst"), the SYST_ACLK rate from its fixed-clock
 * node, and the GPIO port number from the controller's reg offset from gpio0.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define ALIF_CLKCTRL_NODE DT_NODELABEL(clockctrl)

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(dphy))

#define ALIF_CGU_CLK_ENA       (DT_REG_ADDR_BY_NAME(ALIF_CLKCTRL_NODE, cgu) + 0x14)
#define ALIF_CGU_CLK_HFOSC_BIT 21U
#define ALIF_CGU_CLK_CFG_BIT   23U

#define ALIF_VBAT_PWR_CTRL          (DT_REG_ADDR_BY_NAME(ALIF_CLKCTRL_NODE, vbat) + 0x08)
#define ALIF_VBAT_DPHY_PWR_ISO_MASK (BIT(0) | BIT(1) | BIT(4) | BIT(5) | BIT(8) | BIT(9) | BIT(12))

static int alif_mipi_dphy_power_on(void)
{
	sys_set_bit(ALIF_CGU_CLK_ENA, ALIF_CGU_CLK_HFOSC_BIT);
	sys_set_bit(ALIF_CGU_CLK_ENA, ALIF_CGU_CLK_CFG_BIT);
	sys_clear_bits(ALIF_VBAT_PWR_CTRL, ALIF_VBAT_DPHY_PWR_ISO_MASK);
	return 0;
}

SYS_INIT(alif_mipi_dphy_power_on, PRE_KERNEL_1, 0);

#endif /* dphy okay */

#define ALIF_CDC200_NODE DT_NODELABEL(cdc200)

#if defined(CONFIG_DISPLAY_CDC200) && DT_NODE_HAS_STATUS_OKAY(ALIF_CDC200_NODE) &&                 \
	DT_NODE_HAS_PROP(ALIF_CDC200_NODE, clock_frequency)

#define ALIF_SYST_ACLK_HZ       DT_PROP(DT_NODELABEL(syst_aclk), clock_frequency)
#define ALIF_CDC200_PIXCLK_HZ   DT_PROP(ALIF_CDC200_NODE, clock_frequency)
#define ALIF_CDC200_PIXCLK_CTRL (DT_REG_ADDR_BY_NAME(ALIF_CLKCTRL_NODE, clkctl_per_mst) + 0x04)
#define ALIF_CDC200_PIXCLK_DIV_MASK GENMASK(24, 16)
#define ALIF_CDC200_PIXCLK_DIV      DIV_ROUND_CLOSEST(ALIF_SYST_ACLK_HZ, ALIF_CDC200_PIXCLK_HZ)

BUILD_ASSERT(ALIF_CDC200_PIXCLK_DIV >= 2 && ALIF_CDC200_PIXCLK_DIV <= 511,
	     "cdc200 clock-frequency needs a CDC200_PIXCLK_CTRL divider outside 2..511");
BUILD_ASSERT(ALIF_SYST_ACLK_HZ / ALIF_CDC200_PIXCLK_DIV == ALIF_CDC200_PIXCLK_HZ,
	     "cdc200 clock-frequency must be SYST_ACLK / an integer divider: the DSI host "
	     "times its lanes from it, so an unreachable rate overflows the DPI FIFO");
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT < CONFIG_DISPLAY_INIT_PRIORITY,
	     "the CDC200 pixel-clock divider must be set before the display drivers init");
#if defined(CONFIG_MIPI_DSI_DW)
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT < CONFIG_MIPI_DSI_INIT_PRIORITY,
	     "the CDC200 pixel-clock divider must be set before dsi_dw_init() enables that clock");
#endif

static int alif_cdc200_pixclk_div_set(void)
{
	uint32_t v = sys_read32(ALIF_CDC200_PIXCLK_CTRL);

	v = (v & ~ALIF_CDC200_PIXCLK_DIV_MASK) |
	    FIELD_PREP(ALIF_CDC200_PIXCLK_DIV_MASK, ALIF_CDC200_PIXCLK_DIV);
	sys_write32(v, ALIF_CDC200_PIXCLK_CTRL);
	return 0;
}

SYS_INIT(alif_cdc200_pixclk_div_set, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* CONFIG_DISPLAY_CDC200 && cdc200 okay with clock-frequency */

#if defined(CONFIG_MIPI_DSI_DW) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(mipi_dsi))

#define ALIF_GPIO0_NODE DT_NODELABEL(gpio0)
#define ALIF_GPIO_PORT(ctlr)                                                                       \
	((DT_REG_ADDR(ctlr) - DT_REG_ADDR(ALIF_GPIO0_NODE)) / DT_REG_SIZE(ALIF_GPIO0_NODE))
/* gpio0..gpio14 only: lpgpio (0x42002000) is below gpio0 and uses its own pad window. */
#define ALIF_GPIO_IS_MAIN_PORT(ctlr)                                                               \
	(DT_REG_ADDR(ctlr) >= DT_REG_ADDR(ALIF_GPIO0_NODE) && ALIF_GPIO_PORT(ctlr) <= 14)

#define ALIF_BL_PAD_TO_GPIO(node_id)                                                               \
	IF_ENABLED(UTIL_AND(DT_NODE_HAS_PROP(node_id, bl_gpios),                                   \
			    DT_NODE_HAS_COMPAT(DT_GPIO_CTLR(node_id, bl_gpios),                    \
					       snps_designware_gpio)),                             \
		   ({                                                                              \
			   if (ALIF_GPIO_IS_MAIN_PORT(DT_GPIO_CTLR(node_id, bl_gpios))) {          \
				   pinctrl_soc_pin_t pad =                                         \
					   ALIF_PINMUX(ALIF_GPIO_PORT(DT_GPIO_CTLR(node_id,        \
										   bl_gpios)),     \
						       DT_GPIO_PIN(node_id, bl_gpios), 0) |        \
					   ALIF_PAD_CONF_REN(1);                                   \
				   ret = pinctrl_configure_pins(&pad, 1, PINCTRL_REG_NONE);        \
				   if (ret != 0) {                                                 \
					   return ret;                                             \
				   }                                                               \
			   }                                                                       \
		   }))

static int alif_dsi_panel_bl_pad_to_gpio(void)
{
	int ret = 0;

	/*
	 * A shield whose panel/bridge is not itself a DSI peripheral (e.g. the
	 * ti,sn65dsi83 DSI-to-LVDS bridge, controlled over I2C, not DCS) has no
	 * panel@N child of this node to carry bl-gpios -- check the host node
	 * itself too.  A no-op for a shield like e1m_evk_rk055hdmipi4ma0 that
	 * never sets this property here (its hx8394 panel@0 child carries it
	 * instead, and is still reached by the DT_FOREACH_CHILD below).
	 */
	ALIF_BL_PAD_TO_GPIO(DT_NODELABEL(mipi_dsi))

	DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(mipi_dsi), ALIF_BL_PAD_TO_GPIO)
	return ret;
}

SYS_INIT(alif_dsi_panel_bl_pad_to_gpio, PRE_KERNEL_1, 0);

#endif /* CONFIG_MIPI_DSI_DW && mipi_dsi okay */
