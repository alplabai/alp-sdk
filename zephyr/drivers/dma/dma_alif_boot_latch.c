/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the Alif SoC) -- BENCH-UNVERIFIED ======
 *
 * Alif Ensemble M55-HE PL330 DMA2 boot-manager SECURITY LATCH.
 *
 * The PL330 boot-manager samples its boot-security pins ONLY out of reset; a
 * DMAGO issued for an unlatched security domain is silently treated as DMANOP
 * (channel never starts -- SA0=DA0=CPC0=0, no fault).  The sdk-alif fork latches
 * every DMA into the secure domain in its own soc_init
 * (soc/alif/ensemble/common/soc_common.c).  Our build is upstream Zephyr + the
 * alp-sdk module and does NOT run the fork's soc_init, so replicate the DMA2
 * (M55-HE-local) latch here: clear DMA_CTRL[0] (secure), zero the NS IRQ +
 * PERIPH selects, then pulse DMA_CTRL[16] (SW_RST) to re-boot the manager into
 * the secure domain.  Runs PRE_KERNEL_1, before the PL330 driver init
 * (CONFIG_DMA_INIT_PRIORITY).  Only the DMA2 (HE) instance is latched -- that is
 * the controller behind evtrtr2 that the HE-core SPI/peripheral DMA uses.
 *
 * Registers: M55HE_CFG @ 0x43007000 (AE822 DFP rtss_he: HE_DMA_CTRL@+0x0,
 * HE_DMA_IRQ@+0x4, HE_DMA_PERIPH@+0x8).
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/devicetree.h>

/* Only build the latch when the peripheral-flow PL330 is actually in the DT. */
#if DT_HAS_COMPAT_STATUS_OKAY(alif_dma_pl330)

#define M55HE_CFG_HE_DMA_CTRL   0x43007000u
#define M55HE_CFG_HE_DMA_IRQ    0x43007004u
#define M55HE_CFG_HE_DMA_PERIPH 0x43007008u
#define M55HE_CFG_HE_CLK_ENA    0x43007010u
#define HE_CLK_ENA_DMA2_CKEN    BIT(4) /* ALIF_DMA2_CLK: HE_CLK_ENA[4] (DFP sys_ctrl_dma.h) */

static int alp_alif_dma2_boot_latch(void)
{
	/* Ungate the DMA2 peripheral clock FIRST -- the PL330 registers bus-fault
	 * while gated at reset (bench skill), so the security latch below must run
	 * with the clock on. */
	sys_set_bits(M55HE_CFG_HE_CLK_ENA, HE_CLK_ENA_DMA2_CKEN);
	sys_clear_bits(M55HE_CFG_HE_DMA_CTRL, BIT(0)); /* secure domain */
	sys_write32(0u, M55HE_CFG_HE_DMA_IRQ);         /* NS IRQ select   -> none */
	sys_write32(0u, M55HE_CFG_HE_DMA_PERIPH);      /* NS periph select -> none */
	sys_set_bits(M55HE_CFG_HE_DMA_CTRL, BIT(16));  /* SW_RST: re-boot the manager */
	return 0;
}

SYS_INIT(alp_alif_dma2_boot_latch, PRE_KERNEL_1, 0);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(alif_dma_pl330) */
