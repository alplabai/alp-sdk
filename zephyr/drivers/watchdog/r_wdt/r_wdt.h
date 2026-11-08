/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N CM33 WDT0 port; see r_wdt.c)
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/*******************************************************************************************************************//**
 * @addtogroup WDT WDT
 * @{
 **********************************************************************************************************************/

#ifndef R_WDT_H
#define R_WDT_H

#include "bsp_api.h"

#include "r_wdt_cfg.h"
#include "r_wdt_api.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/** WDT private control block. DO NOT MODIFY. Initialization occurs when R_WDT_Open() is called. */
typedef struct st_wdt_instance_ctrl
{
    uint32_t wdt_open;                                 // Indicates whether the open() API has been successfully
                                                       // called.
    R_WDT0_Type * p_reg;                               // Base register for this channel
} wdt_instance_ctrl_t;

/**********************************************************************************************************************
 * Exported global variables
 **********************************************************************************************************************/

/** @cond INC_HEADER_DEFS_SEC */
/** Filled in Interface API structure for this Instance. */
extern const wdt_api_t g_wdt_on_wdt;

/** @endcond */

/**********************************************************************************************************************
 * Public Function Prototypes
 **********************************************************************************************************************/
fsp_err_t R_WDT_Refresh(wdt_ctrl_t * const p_ctrl);

fsp_err_t R_WDT_Open(wdt_ctrl_t * const p_ctrl, wdt_cfg_t const * const p_cfg);

fsp_err_t R_WDT_StatusClear(wdt_ctrl_t * const p_ctrl, const wdt_status_t status);

fsp_err_t R_WDT_StatusGet(wdt_ctrl_t * const p_ctrl, wdt_status_t * const p_status);

fsp_err_t R_WDT_CounterGet(wdt_ctrl_t * const p_ctrl, uint32_t * const p_count);

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif                                 // R_WDT_H

/*******************************************************************************************************************//**
 * @} (end addtogroup WDT)
 **********************************************************************************************************************/
