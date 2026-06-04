/*
 * Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
 * Copyright (c) 2026 Alp Lab AB  (RZ/V2N adaptation shim)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FSP module configuration for the r_sci_b_spi (SCI Simple-SPI) driver, ported
 * from RA to the RZ/V2N (R9A09G056) Cortex-M33.  The upstream FSP ships the
 * r_sci_b_spi module only for the RA family, so the RA-only BSP feature macros
 * it relies on are absent from the rzv BSP.  We supply them here -- in the
 * project config header that the module already includes -- so the upstream
 * r_sci_b_spi.c compiles byte-for-byte unmodified on RZ/V2N (clean `west
 * update` diffs).  See alp-sdk zephyr/drivers/spi/spi_renesas_rz_sci_b.c.
 */

#ifndef R_SCI_B_SPI_CFG_H_
#define R_SCI_B_SPI_CFG_H_
#ifdef __cplusplus
extern "C" {
#endif

/* DMA fast path -- follows the glue driver's ALP_V2N_SCI7_DMAC gate (one
 * build-level switch flips both TUs; pass -DALP_V2N_SCI7_DMAC=1 via
 * EXTRA_CFLAGS).  Bench state 2026-06-04 (vendor-config round): with the
 * e2-studio FSP generator's configuration (MASK_DACK_OUTPUT +
 * NO_DETECTION + SOURCE_MODULE both directions + unused DREQ/DACK/TEND
 * pins -- the 2026-06-03 sweep had REQD inverted on RX and the pin fields
 * zero-initialised to PIN0) the DMAC genuinely STREAMS: full request +
 * reply transactions complete end-to-end through the FSP machinery
 * including the channel-end interrupts and the TEND completion (the GD32
 * received intact frames; the old "parks CHSTAT.SUS after one beat"
 * behaviour is gone).  Remaining blocker: after initial success the TX
 * channel intermittently parks CHSTAT.ER (AXI bus error) one beat into a
 * TDR write through the 0x128xxxxx convert window, while RX reads through
 * the same window keep completing -- a bus-master security/Master-MPU
 * attribution lead (BL2-programmed SYS master-access controls), not a
 * channel-config one.  Park: default-off until that is root-caused on the
 * bench; the 25 MHz production path is the polled FIFO engine.
 * NOTE: the DMA-gated blocks in r_sci_b_spi.c carry documented RZ-port
 * edits -- the rzv BSP overrides transfer_info_t
 * (BSP_OVERRIDE_TRANSFER_INFO_T) with discrete members instead of the RA
 * packed transfer_settings_word, and the RX arm uses reconfigure()
 * because the rzv R_DMAC_B_Reset is an FSP_ERR_UNSUPPORTED stub. */
#ifndef ALP_V2N_SCI7_DMAC
#define ALP_V2N_SCI7_DMAC                   0
#endif
#define SCI_B_SPI_CFG_DMA_SUPPORT_ENABLE    (ALP_V2N_SCI7_DMAC)
#define SCI_B_SPI_CFG_PARAM_CHECKING_ENABLE (BSP_CFG_PARAM_CHECKING_ENABLE)
#define SCI_B_SPI_CFG_FIFO_SUPPORT          (0)

/* DEPRECATED alias still referenced by the upstream module body. */
#define SCI_B_SPI_DTC_SUPPORT_ENABLE        (SCI_B_SPI_CFG_DMA_SUPPORT_ENABLE)

/*
 * RZ/V2N adaptation -- supply the RA-only BSP feature macros the rzv BSP omits:
 *
 *  - The RZ/V2N SCI_B CCR4 register has NO SCKSEL field (verified against
 *    R9A09G056N sci_b_iobitmask.h: CCR4 = CMPD/ASEN/ATEN/AST/AJD/ATT/AET only).
 *    So the "select master receive clock" mask is 0 and the bit position is a
 *    don't-care -- `r_sci_b_spi_hw_config()` then computes ccr4 = (0 << 0) = 0,
 *    i.e. it leaves CCR4 at its reset value, which is correct for this IP.
 *
 *  - The SCI operation clock on RZ/V2N is P5CLK (BSP_FEATURE_SCI_CLOCK), exactly
 *    as r_sci_b_uart uses on this SoC.  spi_renesas_rz_sci_b.c always selects
 *    SCI_B_SPI_SOURCE_CLOCK_PCLK, so R_SCI_B_SPI_CalculateBitrate() always takes
 *    the R_FSP_SystemClockHzGet(BSP_FEATURE_SCI_CLOCK) branch.  The SCISPICLK
 *    branch (R_FSP_SciSpiClockHzGet) is deliberately left disabled -- that
 *    symbol is NOT linked on this rzv2n build (no FSP module references it, and
 *    rzv2n leaves BSP_FEATURE_*_HAS_SCISPI_CLOCK undefined), so referencing it
 *    would be a link error.  The module's `peripheral_clock` local is given a
 *    defined initial value in r_sci_b_spi.c (the one allowed, well-commented
 *    deviation from the upstream source) so the unused branch raises no
 *    -Wmaybe-uninitialized.
 */
#ifndef BSP_FEATURE_SCI_SPI_SCKSEL_MASK
#define BSP_FEATURE_SCI_SPI_SCKSEL_MASK     (0UL)
#endif
#ifndef R_SCI_B0_CCR4_SCKSEL_Pos
#define R_SCI_B0_CCR4_SCKSEL_Pos            (0UL)
#endif

#ifdef __cplusplus
}
#endif
#endif /* R_SCI_B_SPI_CFG_H_ */
