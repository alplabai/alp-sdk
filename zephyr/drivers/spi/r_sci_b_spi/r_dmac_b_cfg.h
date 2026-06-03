/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * (Alp-authored configuration shim -- no Renesas code is copied here, so the
 * alp-sdk Apache-2.0 license applies; the vendored FSP module sources this
 * file configures keep their upstream Renesas BSD-3-Clause license.)
 *
 * FSP module configuration for the rzv r_dmac_b (DMAC-B) transfer driver,
 * compiled from the Zephyr hal_renesas module
 * (drivers/rz/fsp/src/rzv/r_dmac_b/r_dmac_b.c) into the alp-sdk library to
 * provide the SCI7 Simple-SPI DMA fast path on the RZ/V2N Cortex-M33 (the
 * hal_renesas rzv CMake does not build it by default and there is no
 * renesas,rz-dmac-b node in the rzv2n devicetree).  This is the standard FSP
 * <module>_cfg.h contract; it lives next to r_sci_b_spi_cfg.h because both
 * serve the same vendored SCI7-SPI stack and share its include path.
 */

#ifndef R_DMAC_B_CFG_H_
#define R_DMAC_B_CFG_H_
#ifdef __cplusplus
extern "C" {
#endif

#define DMAC_B_CFG_PARAM_CHECKING_ENABLE (BSP_CFG_PARAM_CHECKING_ENABLE)

#ifdef __cplusplus
}
#endif
#endif /* R_DMAC_B_CFG_H_ */
