/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N MHU-B port)
*
* SPDX-License-Identifier: BSD-3-Clause
*
* FSP module configuration for r_mhu_b_ns, mirroring the cfg-header shape of
* the upstream r_mhu_ns_cfg.h (rzv/rzg) so this port stays a drop-in for
* mhu_api_t consumers.  BSP_CFG_PARAM_CHECKING_ENABLE is the project-wide
* rzv2n setting (zephyr/rz/rz_cfg/fsp_cfg/bsp/rzv2n/bsp_cfg.h), same symbol
* r_sci_b_spi_cfg.h references.
*/

#ifndef R_MHU_B_NS_CFG_H_
#define R_MHU_B_NS_CFG_H_
#ifdef __cplusplus
extern "C" {
#endif

#define MHU_B_NS_CFG_PARAM_CHECKING_ENABLE (BSP_CFG_PARAM_CHECKING_ENABLE)

#ifdef __cplusplus
}
#endif
#endif /* R_MHU_B_NS_CFG_H_ */
