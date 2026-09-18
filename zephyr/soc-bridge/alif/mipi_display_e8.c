/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the Alif SoC) -- bench-proven on the aen-dsi-display RAM-run ======
 * SoC-level setup the Alif E8 MIPI-DSI display chain (CDC200 -> DesignWare DSI
 * -> D-PHY) needs and that none of its drivers, nor Zephyr's upstream Alif
 * clock control, performs.  Each step mirrors Alif's own documented sequence;
 * none reimplements a driver.  Built only when the display chain is
 * configured (zephyr/CMakeLists.txt), and each hook is gated on its DT node.
 * See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
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
 *    DevKit-E8 vbat_init(); no Secure Enclave service involved.
 *
 * 2. CDC200 pixel-clock divider (POST_KERNEL, before the display drivers;
 *    cdc200 okay with a clock-frequency).  Upstream clock_control_alif has no
 *    .set_rate for this clock, so CDC200_PIXCLK_CTRL[24:16] keeps its reset
 *    divisor 511 (~0.78 MHz).  The divider is set to SYST_ACLK / the cdc200
 *    clock-frequency (rounded, clamped to 2..511) -- the same property the DSI
 *    host times its lanes from, so the two cannot disagree (when they did,
 *    the DPI payload FIFO overflowed every line).  Other bits are kept; the
 *    enable (bit 0) is set later by the cdc200 driver's clock_control_on().
 *    Register: CLKCTL_PER_MST + 0x04 (AE822 DFP sys_ctrl_cdc.h).
 *
 * 3. DSI panel backlight pad (PRE_KERNEL_1, a mipi_dsi panel child with
 *    bl-gpios).  gpio_dw applies no pad mux, and the panel driver only drives
 *    bl-gpios, so the pad is muxed to its GPIO function (alt 0) with the input
 *    receiver on, so the level also reads back.  The CDC200 driver's own
 *    pinctrl-0 is not applied in a MIPI-DSI build, which is why this lives
 *    here and not in a pinctrl group on &cdc200.
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

#define ALIF_CDC200_PIXCLK_CTRL (DT_REG_ADDR_BY_NAME(ALIF_CLKCTRL_NODE, clkctl_per_mst) + 0x04)
#define ALIF_CDC200_PIXCLK_DIV_MASK GENMASK(24, 16)
#define ALIF_CDC200_PIXCLK_DIV                                                                     \
	CLAMP(DIV_ROUND_CLOSEST(DT_PROP(DT_NODELABEL(syst_aclk), clock_frequency),                  \
				DT_PROP(ALIF_CDC200_NODE, clock_frequency)),                       \
	      2, 511)

BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT < CONFIG_DISPLAY_INIT_PRIORITY,
	     "the CDC200 pixel-clock divider must be set before the display drivers init");

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

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(mipi_dsi))

#define ALIF_GPIO0_NODE DT_NODELABEL(gpio0)
#define ALIF_GPIO_PORT(ctlr)                                                                       \
	((DT_REG_ADDR(ctlr) - DT_REG_ADDR(ALIF_GPIO0_NODE)) / DT_REG_SIZE(ALIF_GPIO0_NODE))

#define ALIF_BL_PAD_TO_GPIO(node_id)                                                               \
	IF_ENABLED(DT_NODE_HAS_PROP(node_id, bl_gpios), ({                                         \
		BUILD_ASSERT(DT_REG_ADDR(DT_GPIO_CTLR(node_id, bl_gpios)) >=                        \
				     DT_REG_ADDR(ALIF_GPIO0_NODE) &&                                \
			     ALIF_GPIO_PORT(DT_GPIO_CTLR(node_id, bl_gpios)) <= 14,                 \
			     "bl-gpios must be on a main-domain GPIO port (gpio0..gpio14)");        \
		pinctrl_soc_pin_t pad =                                                             \
			ALIF_PINMUX(ALIF_GPIO_PORT(DT_GPIO_CTLR(node_id, bl_gpios)),                \
				    DT_GPIO_PIN(node_id, bl_gpios), 0) |                            \
			ALIF_PAD_CONF_REN(1);                                                       \
		ret = pinctrl_configure_pins(&pad, 1, PINCTRL_REG_NONE);                            \
		if (ret != 0) {                                                                     \
			return ret;                                                                 \
		}                                                                                   \
	}))

static int alif_dsi_panel_bl_pad_to_gpio(void)
{
	int ret = 0;

	DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(mipi_dsi), ALIF_BL_PAD_TO_GPIO)
	return ret;
}

SYS_INIT(alif_dsi_panel_bl_pad_to_gpio, PRE_KERNEL_1, 0);

#endif /* mipi_dsi okay */
