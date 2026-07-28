/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the Alif SoC) -- BENCH-UNVERIFIED ======
 * Mirrors the vendor's own documented sequence (Alif DFP
 * drivers/include/sys_ctrl_gpio.h enable_gpio_clk()); not a reimplementation of
 * gpio_dw. See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ===================================================================
 *
 * Alif Ensemble E8 (AE822) DesignWare GPIO functional-clock enable.
 *
 * Each DW GPIO port on AE822 is gated by a per-instance bit in
 * CLKCTL_PER_SLV->GPIO_CTRL[n] (AE822 DFP Device/soc/AE822FA0E5597/include/
 * rtss_he/soc.h:2594, `__IOM uint32_t GPIO_CTRL[15]` at CLKCTL_PER_SLV+0x80).
 * Upstream Zephyr's gpio_dw driver only touches the DW-apb-GPIO's own APB
 * register block (SWPORTA_DDR/SWPORTA_DR/...) and never writes this SoC-level
 * gate -- so without this file the SoC-documented functional-clock enable
 * for the port is simply never asserted.
 *
 * CORRECTION (2026-07-27, same bench, later + decisive): CKEN was ORIGINALLY
 * suspected to be why an E1M-AEN801 LED on P2_4 (`gpio2` pin 4) looked dark
 * under GPIO alt-0 while its UTIMER PWM alt-4 function lit it -- that theory
 * is REFUTED.  A follow-up A/B on the same silicon drove SWPORTA_DDR/
 * SWPORTA_DR from the debugger with CKEN still clear after a cold reset and
 * the pad moved: `0x49002050 = 0x00000010` with `0x4902F088 = 0x00000100`
 * (bit 16 unset).  Bit 16 is measured NOT required for pad drive on AE822.
 * The actual explanation for the dark LED was unrelated to this clock gate
 * (the app returned after a short toggle burst with the pad left LOW, not
 * "the pad can't move without CKEN") -- see docs/aen-bench-bringup.md's GPIO
 * row and examples/aen/aen-gpio-bench/src/main.c for the corrected account.
 * This file's write is kept because it matches Alif's own documented
 * enable_gpio_clk() init sequence (belt-and-braces, ADR 0017 Tier-1.5), NOT
 * because it was shown to fix a dark pad -- it does not.
 *
 * The gate is bit 16 (GPIO_CTRL_CKEN, the *functional* clock) per the Alif DFP
 * (drivers/include/sys_ctrl_gpio.h):
 *
 *   #define GPIO_CTRL_DB_CKEN (1U << 12U)  // debounce clock ONLY -- not this
 *   #define GPIO_CTRL_CKEN    (1U << 16U)  // functional clock -- what we need
 *   enable_gpio_clk(instance): CLKCTL_PER_SLV->GPIO_CTRL[instance] |= GPIO_CTRL_CKEN;
 *
 * guarded there by `#if SOC_FEAT_GPIO_HAS_CLOCK_ENABLE`, which the DFP sets
 * per-part: AE822FA0E5597 soc_features.h:114 -> 1 (this file applies);
 * AE722F80F55D5 (E7) soc_features.h:109 -> 0 (E7 has no such gate -- writing
 * bit 16 there would be wrong).  This file gates on CONFIG_SOC_SERIES_E8, but
 * NOT because E8 is "the only Ensemble series alp-sdk boards" -- alp-sdk also
 * boards E4 (e1m_aen401_m55_hp, AE402FA0E5597) and E6 (e1m_aen601_m55_hp,
 * AE612FA0E5597LS0).  The real reason is narrower: this file is keyed off
 * `DT_HAS_COMPAT_STATUS_OKAY(snps_designware_gpio)` too, and today only the
 * E8 peripherals dtsi (zephyr/dts/alif/ensemble_e8_peripherals.dtsi)
 * instantiates any `snps,designware-gpio` node -- so the compat guard alone
 * already excludes E4/E6, and CONFIG_SOC_SERIES_E8 is redundant-but-cheap
 * belt-and-braces, not a considered "E8 only has the gate" claim.  AE402
 * DOES carry the same gate (AE402FA0E5597/include/soc_features.h:108 ->
 * SOC_FEAT_GPIO_HAS_CLOCK_ENABLE (1); AE1C1F4051920/include/soc_features.h:85
 * also -> 1) -- when an E4 or E6 dtsi grows a `snps,designware-gpio` node,
 * widen the `#if` here (or drop the series guard and rely on the compat
 * check alone) rather than assuming this file already covers it.
 *
 * INSTANCE LIST: derived from DT, not a hardcoded 0..14 loop.  Every
 * `snps,designware-gpio` node with status = "okay" contributes its `reg`
 * address; instance = (reg - 0x49000000) / 0x1000 (gpio0 is the base at
 * 0x49000000, each port is one 0x1000 block -- zephyr/dts/alif/
 * ensemble_e8_peripherals.dtsi gpio0..gpio10).  alp-sdk's own
 * CLKCTRL_PER_SLV_GPIO_CTRLn base (zephyr/soc-bridge/alif/soc_common.h) is the
 * same derivation already recorded for this SoC family.
 *
 * LPGPIO IS EXCLUDED, NOT MIS-HANDLED: `lpgpio` (gpio@42002000, DFP
 * LPGPIO_BASE) is a DW GPIO node too but is NOT one of the 15
 * CLKCTL_PER_SLV->GPIO_CTRL[] entries, and its gate is not identified
 * anywhere in the DFP -- `sys_ctrl_gpio.h` never mentions LPGPIO at all.
 * lpgpio's reg (0x42002000) falls outside the 0x49000000..0x4900e000 window
 * this file computes instances from, so it is skipped by construction
 * (alp_alif_gpio_clk_enable() bounds-checks the offset and instance count
 * below) on address grounds alone -- not because its gate is known and
 * handled elsewhere.
 *
 * GPIO16/GPIO17 ARE ALSO EXCLUDED BY THE SAME GUARD, and for THESE the DFP
 * does name the gate: GPIO16/GPIO17 are two more DW GPIO blocks, separate
 * from both the CLKCTL_PER_SLV->GPIO_CTRL[15] array and from lpgpio, at
 * GPIO16_BASE 0x4300A000 / GPIO17_BASE 0x4300B000
 * (AE822FA0E5597/include/rtss_he/soc.h:3689-3690) -- also outside this
 * file's 0x49000000..0x4900e000 window, so they too are skipped by
 * construction.  The DFP's enable_gpio_clk() special-cases exactly these two
 * instances (GPIO16_INSTANCE/GPIO17_INSTANCE) onto AON->RTSS_HE_LPUART_CKEN
 * bits 10/11 instead of a GPIO_CTRL[] entry.  No E1M-AEN801 carrier pad
 * routes to GPIO16/GPIO17 (or to lpgpio) today; wire the
 * AON_RTSS_HE_LPUART_CKEN gate here if a route to GPIO16/GPIO17 ever lands --
 * lpgpio's own gate is still unidentified, so there is nothing to wire for it.
 *
 * INIT LEVEL: PRE_KERNEL_1, priority 1 -- deliberately earlier than gpio_dw's
 * own init (PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, default
 * KERNEL_INIT_PRIORITY_DEFAULT = 40).  The clock must be enabled before
 * gpio_dw's first register touch (or before any alp_gpio_open()); this is not
 * "usually earlier" but a hard PRE_KERNEL_1-ordered dependency, chosen the same
 * way as the DMA2 boot latch beside it (dma_alif_boot_latch.c, priority 0)
 * runs before CONFIG_DMA_INIT_PRIORITY -- a shared/system init a device driver
 * silently depends on reads as "works sometimes" if left to priority ties.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_SERIES_E8) && DT_HAS_COMPAT_STATUS_OKAY(snps_designware_gpio)

/* CLKCTL_PER_SLV->GPIO_CTRL[0]; instance n is this + 4*n (AE822 DFP
 * rtss_he/soc.h:2594; alp-sdk's own CLKCTRL_PER_SLV_GPIO_CTRLn in
 * zephyr/soc-bridge/alif/soc_common.h uses the same 0x4902F080 base). */
#define ALIF_GPIO_CTRL_BASE   0x4902F080u
#define ALIF_GPIO_CTRL_STRIDE 4u
#define ALIF_GPIO_CTRL_COUNT  15u /* CLKCTL_PER_SLV_Type::GPIO_CTRL[15] */
/* GPIO_CTRL_CKEN (bit 16) -- the functional clock. NOT bit 12 (GPIO_CTRL_DB_CKEN,
 * debounce-only): enabling that instead/also would silently change input
 * debounce behaviour and does not gate the block this bug is about. */
#define ALIF_GPIO_CTRL_CKEN BIT(16)

/* gpio0's own reg base; each DW GPIO port occupies one 0x1000 block
 * (ensemble_e8_peripherals.dtsi gpio0@0x49000000 .. gpio10@0x4900a000). */
#define ALIF_GPIO_PORT_BASE   0x49000000u
#define ALIF_GPIO_PORT_STRIDE 0x1000u

/* Enable the functional clock for one DW GPIO port, given its DT `reg`
 * address.  Silently skips anything outside the 15-entry GPIO_CTRL[] window
 * (lpgpio at 0x42002000 included) instead of writing a miscomputed bit. */
static void alp_alif_gpio_clk_enable(uint32_t port_reg_addr)
{
	if (port_reg_addr < ALIF_GPIO_PORT_BASE) {
		return;
	}

	uint32_t offset = port_reg_addr - ALIF_GPIO_PORT_BASE;

	if ((offset % ALIF_GPIO_PORT_STRIDE) != 0) {
		return;
	}

	uint32_t instance = offset / ALIF_GPIO_PORT_STRIDE;

	if (instance >= ALIF_GPIO_CTRL_COUNT) {
		return;
	}

	sys_set_bits(ALIF_GPIO_CTRL_BASE + (instance * ALIF_GPIO_CTRL_STRIDE), ALIF_GPIO_CTRL_CKEN);
}

#define ALP_ALIF_GPIO_CLK_ENABLE_ONE(node_id) alp_alif_gpio_clk_enable(DT_REG_ADDR(node_id));

static int alp_alif_gpio_clk_init(void)
{
	DT_FOREACH_STATUS_OKAY(snps_designware_gpio, ALP_ALIF_GPIO_CLK_ENABLE_ONE)

	return 0;
}

SYS_INIT(alp_alif_gpio_clk_init, PRE_KERNEL_1, 1);

#endif /* CONFIG_SOC_SERIES_E8 && DT_HAS_COMPAT_STATUS_OKAY(snps_designware_gpio) */
