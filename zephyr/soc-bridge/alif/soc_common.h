/*
 * Copyright (c) 2025 Alif Semiconductor
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * vendor-ext / build-bridge.  Alif Ensemble SoC-common register defines.
 *
 * WHY THIS LIVES IN alp-sdk: hal_alif v2.2.0 builds
 * drivers/dma_event_router/src/dma_event_router.c *unconditionally* for the
 * Ensemble SoC family (drivers/CMakeLists.txt: bare `add_subdirectory(...)`),
 * and that TU does `#include <soc_common.h>`.  Alif's own zephyr_alif fork
 * ships this header at soc/alif/ensemble/common/soc_common.h, but UPSTREAM
 * Zephyr v4.4's (more minimal) Alif SoC layer does NOT -- so hal_alif v2.2.0
 * does not build against upstream Zephyr + alp-sdk's no-fork basis without it.
 *
 * alp-sdk therefore carries this header (verbatim from the Apache-2.0 fork,
 * Alif's copyright retained) and puts it on the include path ONLY for Ensemble
 * builds (zephyr/CMakeLists.txt, gated on CONFIG_SOC_FAMILY_ENSEMBLE* so it
 * cannot shadow another vendor's soc_common.h, e.g. NXP's).  Remove this
 * bridge once upstream Zephyr's Alif SoC layer provides soc_common.h or
 * hal_alif gates dma_event_router behind a Kconfig.  Source:
 * github.com/alifsemi/zephyr_alif soc/alif/ensemble/common/soc_common.h.
 */

#ifndef _SOC_COMMON_H_
#define _SOC_COMMON_H_

/* CGU registers. */
#define CGU_BASE                                0x1A602000
#define CGU_PLL_CLK_SEL                         (CGU_BASE + 0x8)
#define CGU_CLK_ENA                             (CGU_BASE + 0x14)

/* AON registers. */
#define AON_BASE                                 0x1A604000
#define AON_RTSS_HP_CTRL                         (AON_BASE + 0x0)
#define AON_RTSS_HP_RESET                        (AON_BASE + 0x4)
#define AON_RTSS_HE_CTRL                         (AON_BASE + 0x10)
#define AON_RTSS_HE_RESET                        (AON_BASE + 0x14)
#define AON_RTSS_HE_LPPERI_CKEN                  (AON_BASE + 0x1C)

/* VBAT registers. */
#define VBAT_BASE                                0x1A609000
#define VBAT_GPIO_CTRL_EN                        (VBAT_BASE)
#define VBAT_PWR_CTRL                            (VBAT_BASE + 0x8)
#define VBAT_LPRTC0_CLK_EN                       (VBAT_BASE + 0x10)
#define VBAT_LPRTC1_CLK_EN                       (VBAT_BASE + 0x14)

/* Expansion Slave registers. */
#define CLKCTRL_PER_SLV_BASE                     0x4902F000
#define CLKCTRL_PER_SLV_UART_CTRL                (CLKCTRL_PER_SLV_BASE + 0x8)
#define CLKCTRL_PER_SLV_SSI_CTRL                 (CLKCTRL_PER_SLV_BASE + 0x28)
#define CLKCTRL_PER_SLV_ADC_CTRL                 (CLKCTRL_PER_SLV_BASE + 0x30)
#define CLKCTRL_PER_SLV_DAC_CTRL                 (CLKCTRL_PER_SLV_BASE + 0x34)
#define CLKCTRL_PER_SLV_CMP_CTRL                 (CLKCTRL_PER_SLV_BASE + 0x38)
#define CLKCTRL_PER_SLV_OSPI_CTRL                (CLKCTRL_PER_SLV_BASE + 0x3C)
#define CLKCTRL_PER_SLV_GPIO_CTRLn               (CLKCTRL_PER_SLV_BASE + 0x80)

/*
 * HSCMP internal-reference blocks.  On AE822 (SOC_FEAT_HSCMP_REG_ALIASING=1,
 * alif-dfp Device/soc/AE822FA0E5597/include/soc_features.h:106) the DAC6
 * programmable reference and the ADC VREF buffer are SEPARATE single-register
 * blocks (not CMP_COMP_REG2) -- the HSCMP DAC6-reference path writes those,
 * not the CMP block.  Bases transcribed from
 * alif-dfp Device/soc/AE822FA0E5597/include/rtss_he/soc.h:3744-3745
 * (DAC6_BASE 0x4902A000 / ADC_VREF_BASE 0x4902B000); each holds one register at
 * offset 0x00 (soc.h:2528 DAC6_REG / soc.h:2542 ADC_VREF_REG).
 */
#define DAC6_BASE                                0x4902A000
#define DAC6_REG                                 (DAC6_BASE)
#define ADC_VREF_BASE                            0x4902B000
#define ADC_VREF_REG                             (ADC_VREF_BASE)

#define EVTRTR0_BASE                             0x49035000
#define EVTRTR0_DMA_CTRL0                        (EVTRTR0_BASE)
#define EVTRTR0_DMA_REQ_CTRL                     (EVTRTR0_BASE + 0x80)
#define EVTRTR0_DMA_ACK_TYPE0                    (EVTRTR0_BASE + 0x90)

#define EVTRTRLOCAL_BASE                         0x400E2000
#define EVTRTRLOCAL_DMA_CTRL0                    (EVTRTRLOCAL_BASE)
#define EVTRTRLOCAL_DMA_REQ_CTRL                 (EVTRTRLOCAL_BASE + 0x80)
#define EVTRTRLOCAL_DMA_ACK_TYPE0                (EVTRTRLOCAL_BASE + 0x90)

/* NB: the EVTRTR2_DMA_CTRL_* bit masks dma_event_router.c also uses are
 * defined by hal_alif's own drivers/dma_event_router/include/dma_event_router.h,
 * not here -- this header only needs to supply the EVTRTRLOCAL_* bases above.
 */

/* Expansion Master-0 registers. */
#define CLKCTRL_PER_MST_BASE                     0x4903F000
#define CLKCTRL_PER_MST_CAMERA_PIXCLK_CTRL       (CLKCTRL_PER_MST_BASE)
#define CLKCTRL_PER_MST_CDC200_PIXCLK_CTRL       (CLKCTRL_PER_MST_BASE + 0x4)
#define CLKCTRL_PER_MST_CSI_PIXCLK_CTRL          (CLKCTRL_PER_MST_BASE + 0x8)
#define CLKCTRL_PER_MST_PERIPH_CLK_EN            (CLKCTRL_PER_MST_BASE + 0xC)
#define CLKCTRL_PER_MST_MIPI_CKEN                (CLKCTRL_PER_MST_BASE + 0x40)
#define CLKCTRL_PER_MST_DMA_CTRL                 (CLKCTRL_PER_MST_BASE + 0x70)
#define CLKCTRL_PER_MST_DMA_IRQ                  (CLKCTRL_PER_MST_BASE + 0x74)
#define CLKCTRL_PER_MST_DMA_PERIPH               (CLKCTRL_PER_MST_BASE + 0x78)
#define CLKCTRL_PER_MST_USB_CTRL2                (CLKCTRL_PER_MST_BASE + 0xAC)

/* M55-HE Config registers. */
#define M55HE_CFG_HE_CFG_BASE                    0x43007000
#define M55HE_CFG_HE_DMA_CTRL                    (M55HE_CFG_HE_CFG_BASE)
#define M55HE_CFG_HE_DMA_IRQ                     (M55HE_CFG_HE_CFG_BASE + 0x4)
#define M55HE_CFG_HE_DMA_PERIPH                  (M55HE_CFG_HE_CFG_BASE + 0x8)
#define M55HE_CFG_HE_DMA_SEL                     (M55HE_CFG_HE_CFG_BASE + 0xC)
#define M55HE_CFG_HE_CLK_ENA                     (M55HE_CFG_HE_CFG_BASE + 0x10)

/* HE_DMA_SEL bit-field: LPSPI DMA selection (bits [5:4]) */
#define HE_DMA_SEL_LPSPI_Pos                     4U
#define HE_DMA_SEL_LPSPI_Msk                     (0x3U << HE_DMA_SEL_LPSPI_Pos)
#define M55HE_CFG_HE_CAMERA_PIXCLK               (M55HE_CFG_HE_CFG_BASE + 0x20)

/* M55-HP Config registers. */
#define M55HP_CFG_HP_CFG_BASE                    0x400F0000
#define M55HP_CFG_HP_DMA_CTRL                    (M55HP_CFG_HP_CFG_BASE)
#define M55HP_CFG_HP_DMA_IRQ                     (M55HP_CFG_HP_CFG_BASE + 0x4)
#define M55HP_CFG_HP_DMA_PERIPH                  (M55HP_CFG_HP_CFG_BASE + 0x8)
#define M55HP_CFG_HP_DMA_SEL                     (M55HP_CFG_HP_CFG_BASE + 0xC)
#define M55HP_CFG_HP_CLK_ENA                     (M55HP_CFG_HP_CFG_BASE + 0x10)

/* ANA Register */
#define ANA_BASE                                 0x1A60A000
#define ANA_VBAT_REG1                            (ANA_BASE + 0x38)
#define ANA_VBAT_REG2                            (ANA_BASE + 0x3C)

/* LPGPIO Base address for LPTIMER pin config */
#define LPGPIO_BASE                              0x42002008

/* lptimer helper macro */
#define LPTIMER_CONFIG(idx)						\
	/* Check if timer node is enabled in DT */			\
	IF_ENABLED(DT_NODE_HAS_STATUS(DT_NODELABEL(timer##idx), okay), (\
		if (IS_ENABLED(CONFIG_LPTIMER##idx##_OUTPUT_TOGGLE) ||	\
			(CONFIG_LPTIMER##idx##_EXT_CLK_FREQ > 0U)) {	\
			/* enable LPTIMER##idx pin via LPGPIO */	\
			sys_set_bit(LPGPIO_BASE, idx);			\
		}							\
	))

#endif /* _SOC_COMMON_H_ */
