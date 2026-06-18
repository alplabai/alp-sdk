/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-sdk extension clock IDs for the Alif Ensemble, in the UPSTREAM 8-arg
 * ALIF_CLK_CFG() encoding (zephyr/dt-bindings/clock/alif-clocks-common.h).
 *
 * Upstream Zephyr v4.4's alif-ensemble-clocks.h defines ONLY the 8 UART
 * clocks; the peripheral clock IDs below are re-expressed from Alif's
 * Apache-2.0 zephyr_alif fork into the upstream encoding.  This re-authoring
 * is load-bearing, NOT a transcription: the fork and upstream encodings
 * differ in two ways that both matter --
 *
 *   1. arity: fork ALIF_CLK_CFG() takes 7 args, upstream takes 8 (adds a
 *      trailing parent_clk field at bits [26:30]); the fork macro body will
 *      not even compile against the upstream macro.
 *   2. module IDs are off by +1: fork CLKCTL_PER_MST=0x2 / M55HE=0x6 vs
 *      upstream CLKCTL_PER_MST=0x1 / M55HE=0x5.  The module index occupies
 *      the same low bits in both, so a *packed* fork value survives copy but
 *      then selects the WRONG register block under the upstream decode driver
 *      (drivers/clock_control/clock_control_alif.c): e.g. the fork's
 *      ALIF_ETHERNET_CLK (0x00016062) decodes to clkctl_per_slv 0x4902F00C,
 *      not the intended GMAC gate clkctl_per_mst 0x4903F00C.
 *
 * Therefore every ID here uses the upstream ALIF_CLK_CFG() macro so the
 * upstream ALIF_*_MODULE constants get pasted in; the register offset +
 * enable bit (identical silicon, identical in both trees) are carried over
 * from the fork's alif_ensemble_clocks.h.  vendor-ext, BENCH-UNVERIFIED.
 */
#ifndef ALP_DT_BINDINGS_CLOCK_ALIF_ENSEMBLE_CLOCKS_EXT_H_
#define ALP_DT_BINDINGS_CLOCK_ALIF_ENSEMBLE_CLOCKS_EXT_H_

#include <zephyr/dt-bindings/clock/alif-clocks-common.h>

/* Register offsets absent from upstream's header (verified vs fork
 * alif_clocks_common.h: PERIPH_CLK_ENA in CLKCTL_PER_MST, HE_CLK_ENA in
 * M55HE_CFG).  Same silicon, same offsets in both trees. */
#define ALIF_PERIPH_CLK_ENA_REG 0x0CU /* CLKCTL_PER_MST base 0x4903F000 */
#define ALIF_HE_CLK_ENA_REG     0x10U /* M55HE_CFG      base 0x43007000 */

/* I2S0..I2S3 control registers in CLKCTL_PER_SLV (base 0x4902F000); offsets
 * carried from the fork's alif_ensemble_clocks.h (I2S0=0x10 .. I2S3=0x1C, the
 * +4 stride).  Each holds the I2S clock-source select (bit 16) + the bit-clock
 * divider field. */
#define ALIF_I2S0_CTRL_REG 0x10U
#define ALIF_I2S1_CTRL_REG 0x14U
#define ALIF_I2S2_CTRL_REG 0x18U
#define ALIF_I2S3_CTRL_REG 0x1CU

/* Dummy-clock scaffolding (upstream omits it; fork alif_clocks_common.h).
 * DUMMY module = 0x0, en_mask = 0 -> alif_clock_control_on() returns at the
 * `if (!EN_MASK)` guard and touches no register. */
#define ALIF_DUMMY_MODULE 0x0U
#define ALIF_DUMMY_REG    0xFFU
#define ALIF_CLK(value)   ALIF_CLK_CFG(DUMMY, DUMMY, (value), 0U, 0U, 0U, 0U, 0U)

/* Ethernet (GMAC) peripheral clock gate: bit 12 of PERIPH_CLK_ENA in
 * CLKCTL_PER_MST -> sets bit 12 of 0x4903F00C.  parent_clk is used only by
 * clock_control_get_rate(); the dwmac glue does not consume the reported rate
 * for a fixed-link bring-up, so SYST_HCLK is a safe filler. */
#define ALIF_ETHERNET_CLK                                                      \
	ALIF_CLK_CFG(CLKCTL_PER_MST, PERIPH_CLK_ENA, 12U, 1U, 0U, 0U, 0U,       \
		     ALIF_PARENT_CLK_SYST_HCLK)

/* Regular SPI0-3 (DesignWare SSI on the AHB) -- frequency-only dummy clock. */
#define ALIF_SPI_CLK ALIF_CLK(2U)

/* UTIMER -- frequency-only dummy (no software gate reg). The real per-timer tick
 * rate is carried on the utimer node's `clock-frequency` property (TBD vs TRM),
 * not this id; this is the ALIF_SPI_CLK-style placeholder. */
#define ALIF_UTIMER_CLK ALIF_CLK(2U)

/* LPSPI (SPI4, M55-HE local domain): bit 16 of HE_CLK_ENA in M55HE_CFG ->
 * sets bit 16 of 0x43007010.  HE-core only. */
#define ALIF_LPSPI_CLK                                                         \
	ALIF_CLK_CFG(M55HE_CFG, HE_CLK_ENA, 16U, 1U, 0U, 0U, 0U,               \
		     ALIF_PARENT_CLK_SYST_HCLK)

/* LPPDM (low-power PDM mic block, M55-HE local domain): bit 8 of HE_CLK_ENA in
 * M55HE_CFG, with a 1-bit clock-source field at bit 9 (src=0 selects 76.8 MHz;
 * src=1 would be ALIF_LPPDM_AUDIO_CLK).  Re-authored from the fork 7-arg
 * ALIF_CLK_CFG(M55HE_CFG, HE_CLK_ENA, 8U, 1U, 0U, 1U, 9U)
 * (alif_ensemble_clocks.h:93) into the upstream 8-arg encoding -- same en_bit /
 * en_mask / src / src_field, plus a parent_clk filler (the alif,alif-pdm driver
 * only clock_control_configure()/on()s this id, never get_rate()s, so the parent
 * is unused).  HE-core only. */
#define ALIF_LPPDM_76M8_CLK                                                    \
	ALIF_CLK_CFG(M55HE_CFG, HE_CLK_ENA, 8U, 1U, 0U, 1U, 9U,                \
		     ALIF_PARENT_CLK_SYST_HCLK)

/* HP PDM (pdm@4902d000, the main-domain PDM the E1M-AEN801 SoM routes its mics
 * to -- from-alif.tsv): bit 8 of EXPMST0_CTRL (offset 0x0) in CLKCTL_PER_SLV,
 * src-select at bit 9 (src=0 -> 76.8 MHz).  Re-authored from the fork 7-arg
 * ALIF_CLK_CFG(CLKCTL_PER_SLV, EXPMST0_CTRL, 8U, 1U, 0U, 1U, 9U) into the
 * upstream 8-arg encoding; parent_clk is a filler (the alif,alif-pdm driver only
 * configure()/on()s, never get_rate()s). */
#define ALIF_EXPMST0_CTRL_REG 0x00U /* CLKCTL_PER_SLV base 0x4902F000 */
#define ALIF_PDM_76M8_CLK                                                      \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, EXPMST0_CTRL, 8U, 1U, 0U, 1U, 9U,         \
		     ALIF_PARENT_CLK_SYST_HCLK)

/* I2S0..I2S3 block clocks (DesignWare I2S, drivers/i2s/i2s_dw.c).  Re-authored
 * from the fork 7-arg ALIF_CLK_CFG(CLKCTL_PER_SLV, I2Sx_CTRL, 12U, 1U, 0U, 1U,
 * 16U) (alif_ensemble_clocks.h) into the upstream 8-arg encoding: gate bit 12 of
 * the I2Sx_CTRL register in CLKCTL_PER_SLV, with a 1-bit clock-source field at
 * bit 16 (src=0 selects the 76.8 MHz reference; the *_AUDIO variant src=1 would
 * select the external audio PLL, not re-authored here).  parent_clk is a filler:
 * i2s_dw.c derives its bit clock via clock_control_set_rate() (not get_rate()),
 * and our upstream clockctrl has no .set_rate -- so neither the parent nor the
 * divider is consulted by get_rate().  See the i2s_dw.c set_rate note.  Per
 * [[reference_alif_clock_encoding_fork_vs_upstream]] the packed fork value must
 * NOT be copied; the module/reg/bit are re-pasted via the upstream macro. */
#define ALIF_I2S0_76M8_CLK                                                     \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, I2S0_CTRL, 12U, 1U, 0U, 1U, 16U,          \
		     ALIF_PARENT_CLK_SYST_HCLK)
#define ALIF_I2S1_76M8_CLK                                                     \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, I2S1_CTRL, 12U, 1U, 0U, 1U, 16U,          \
		     ALIF_PARENT_CLK_SYST_HCLK)
#define ALIF_I2S2_76M8_CLK                                                     \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, I2S2_CTRL, 12U, 1U, 0U, 1U, 16U,          \
		     ALIF_PARENT_CLK_SYST_HCLK)
#define ALIF_I2S3_76M8_CLK                                                     \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, I2S3_CTRL, 12U, 1U, 0U, 1U, 16U,          \
		     ALIF_PARENT_CLK_SYST_HCLK)

/*
 * CAMERA CAPTURE clocks (CPI / CSI / D-PHY) -- FREQUENCY-ONLY DUMMY PLACEHOLDERS.
 * The fork drives these blocks with ALIF_CPI_CLK / ALIF_CSI_CLK /
 * ALIF_CSI_PIX_SYST_ACLK / ALIF_MIPI_{PLLREF,BYPASS,TXDPHY,RXDPHY}_CLK in the
 * fork's 7-arg encoding.  Per [[reference_alif_clock_encoding_fork_vs_upstream]]
 * those packed values MUST NOT be copied onto the upstream clockctrl, and the
 * upstream-encoding gate/offset/parent for the camera clocks are a TRM/bench
 * unknown -- NOT re-authored here.  These IDs use the dummy clock (ALIF_CLK,
 * module 0x0 / en_mask 0 -> alif_clock_control_on() returns at the !EN_MASK
 * guard, touches no register), so the camera DT nodes are well-formed without
 * inventing a register gate.  The drivers are now PORTED to the v4.4 video API
 * (the ALP_VIDEO_ALIF_BROKEN gate is retired), but no real clock is programmed
 * (the dummy clock above).  Re-author the real IDs when the camera stack is
 * bench-brought-up (task #21).  vendor-ext, BENCH-UNVERIFIED.
 */
#define ALIF_CPI_CLK      ALIF_CLK(2U)
#define ALIF_CSI_CLK      ALIF_CLK(2U)
#define ALIF_CSI_DPHY_CLK ALIF_CLK(2U)

/* CAN-FD (CANFD0, cast,can @0x49036000) peripheral clock.  Re-authored from the
 * Alif DFP CMSIS sys_ctrl_canfd.h (the SOC_FEAT_CANFD0_CANFD1_CTRL=0 / single-
 * CANFD E8 layout) into the upstream 8-arg clockctrl encoding: the CANFD_CTRL
 * register (offset 0xC) in CLKCTL_PER_SLV holds CANFD0's clock-enable at bit 12
 * (CANFD0_CTRL_CKEN, sys_ctrl_canfd.h:49) and a 1-bit clock-source select at
 * bit 16 (CANFD0_CTRL_CLK_SEL_Pos, sys_ctrl_canfd.h:48); src_val=1 selects the
 * 160 MHz source (vs 0 = 38.4 MHz).  The CKDIV field (bits 0..) is left at reset
 * (the controller bit-timing uses the node's clock-frequency).  Per
 * [[reference_alif_clock_encoding_fork_vs_upstream]] the fork's 7-arg packed
 * ALIF_CLK_CFG(CLKCTL_PER_SLV, CANFD_CTRL, 8U, 1U, 1U, 1U, 9U) (its e1c clock
 * header) must NOT be copied -- its en_bit/src_pos (8/9) are the e1c layout, not
 * the E8 DFP layout used here (12/16).  parent_clk is a filler: the cast,can
 * driver configure()/set_rate()/on()s this id but our upstream clockctrl has no
 * .set_rate, so get_rate()'s parent is unused.  vendor-ext, BENCH-UNVERIFIED. */
#define ALIF_CANFD_CTRL_REG 0x0CU /* CLKCTL_PER_SLV base 0x4902F000 */
#define ALIF_CANFD0_160M_CLK                                                   \
	ALIF_CLK_CFG(CLKCTL_PER_SLV, CANFD_CTRL, 12U, 1U, 1U, 1U, 16U,         \
		     ALIF_PARENT_CLK_SYST_HCLK)

#endif /* ALP_DT_BINDINGS_CLOCK_ALIF_ENSEMBLE_CLOCKS_EXT_H_ */
