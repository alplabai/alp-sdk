/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N CM33 WDT0 port)
*
* SPDX-License-Identifier: BSD-3-Clause
*
* FSP module configuration for r_wdt, mirroring the cfg-header shape of the
* hal_renesas rza/r_wdt_cfg.h.  BSP_CFG_PARAM_CHECKING_ENABLE is the project-wide
* rzv2n setting (zephyr/rz/rz_cfg/fsp_cfg/bsp/rzv2n/bsp_cfg.h).
*/

#ifndef R_WDT_CFG_H_
#define R_WDT_CFG_H_
#ifdef __cplusplus
extern "C" {
#endif

#define WDT_CFG_PARAM_CHECKING_ENABLE (BSP_CFG_PARAM_CHECKING_ENABLE)

#ifdef __cplusplus
}
#endif
#endif /* R_WDT_CFG_H_ */
