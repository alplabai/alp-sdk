/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N CM33 WDT0 port)
*
* SPDX-License-Identifier: BSD-3-Clause
*
* ====== ADR 0017 Tier-1.5 (vendored FSP r_wdt, adapted for rzv) -- BENCH-UNVERIFIED ======
* hal_renesas ships r_wdt only for rza/rzn/rzt; the rzv tree has no r_wdt module.  The RZ/V2N
* WDT0 register block (hal_renesas rzv/.../R9A09G056N/iodefines/wdt_iodefine.h, R_WDT0_BASE
* 0x41C00400) has the rzt/rzn layout (WDTRR/WDTCR/WDTSR/WDTRCR/WDTCSTPR), so this is the rzt
* r_wdt.c with these deliberate differences:
*   - register members carry the rzv iodefine WDT0_ prefix (WDT0_WDTRR, ...); the channel is fixed to R_WDT0 (the Cortex-M33's own watchdog); the rzt BSP_CFG_CORE_CR52 /
*     BSP_CFG_CORE_CA55 instance selection has no CM33 case.
*   - R_BSP_MODULE_START(FSP_IP_WDT, 0) releases the WDT0 clock/reset/bus-stop (rzv2n bsp_override.h).
*   - cfg->reset_control / cfg->stop_control are written to WDTRCR.RSTIRQS / WDTCSTPR.SLCSTP
*     (rzt only ever clears RSTIRQS, routing the error to the ICU, which rzv2n does not have).
*   - no ISR/callback: hal_renesas rzv2n bsp_irq_id.h names no WDT interrupt line for the CM33
*     (only the ELC event ELC_EVENT_IWDT_ELCWUN_CM33), so this port is reset-only.
*   - R_WDT_TimeoutGet() dropped (it reads R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKL), which is not
*     the WDT0 counting clock on this SoC); the Zephyr glue takes the clock from devicetree.
* ============================================================================
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_wdt.h"
#include "bsp_api.h"
#include "bsp_cfg.h"

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

#define WDT_OPEN    (0X00574454ULL)

/* Lookup functions for WDT settings.  Using function like macro for stringification. */
#define WDT_PRV_WDTCR_SETTING_GET(setting,                                                   \
                                  wdtcr)    (((wdtcr >> WDT_PRV_WDTCR_ ## setting ## _BIT) & \
                                              WDT_PRV_WDTCR_ ## setting ## _MASK));
#define WDT_PRV_WDTCR_SETTING_SET(setting,                                                   \
                                  value)    ((value & WDT_PRV_WDTCR_ ## setting ## _MASK) << \
                                             WDT_PRV_WDTCR_ ## setting ## _BIT);

/* WDT register settings. */
#define WDT_PRV_WDTSR_COUNTER_MASK           (0x3FFFU)
#define WDT_PRV_WDTSR_FLAGS_MASK             (0xC000U)

#define WDT_PRV_WDTCR_TIMEOUT_BIT            (0)
#define WDT_PRV_WDTCR_CLOCK_DIVISION_BIT     (4)
#define WDT_PRV_WDTCR_WINDOW_END_BIT         (8)
#define WDT_PRV_WDTCR_WINDOW_START_BIT       (12)

#define WDT_PRV_WDTRCR_RESET_CONTROL_BIT     (7)
#define WDT_PRV_WDTCSTPR_STOP_CONTROL_BIT    (7)

#define WDT_PRV_WDTCR_TIMEOUT_MASK           (0x3U) // Bits 0-1
#define WDT_PRV_WDTCR_CLOCK_DIVISION_MASK    (0xFU) // Bits 4-7
#define WDT_PRV_WDTCR_WINDOW_END_MASK        (0x3U) // Bits 8-9
#define WDT_PRV_WDTCR_WINDOW_START_MASK      (0x3U) // Bits 12-13

/* Refresh register values */
#define WDT_PRV_REFRESH_STEP_1               (0U)
#define WDT_PRV_REFRESH_STEP_2               (0xFFU)

/***********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t r_wdt_parameter_checking(wdt_instance_ctrl_t * const p_instance_ctrl, wdt_cfg_t const * const p_cfg);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static const uint8_t g_wdtcr_timeout[] =
{
    0xFFU,                             // WDTCR value for WDT_TIMEOUT_128 (not supported by WDT).
    0xFFU,                             // WDTCR value for WDT_TIMEOUT_512 (not supported by WDT).
    0x00U,                             // WDTCR value for WDT_TIMEOUT_1024.
    0xFFU,                             // WDTCR value for WDT_TIMEOUT_2048 (not supported by WDT).
    0x01U,                             // WDTCR value for WDT_TIMEOUT_4096.
    0x02U,                             // WDTCR value for WDT_TIMEOUT_8192.
    0x03U,                             // WDTCR value for WDT_TIMEOUT_16384.
};

/** Watchdog implementation of WDT Driver  */
const wdt_api_t g_wdt_on_wdt =
{
    .open        = R_WDT_Open,
    .refresh     = R_WDT_Refresh,
    .statusGet   = R_WDT_StatusGet,
    .statusClear = R_WDT_StatusClear,
    .counterGet  = R_WDT_CounterGet,
};

/*******************************************************************************************************************//**
 * @addtogroup WDT WDT
 * @{
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Configure the WDT in register start mode. Implements
 * @ref wdt_api_t::open.
 *
 * This function should only be called once as WDT configuration registers can only be written to once so subsequent
 * calls will have no effect.
 *
 * @retval FSP_SUCCESS              WDT successfully configured.
 * @retval FSP_ERR_ASSERTION        Null pointer, or one or more configuration options is invalid.
 * @retval FSP_ERR_ALREADY_OPEN     Module is already open.  This module can only be opened once.
 **********************************************************************************************************************/
fsp_err_t R_WDT_Open (wdt_ctrl_t * const p_ctrl, wdt_cfg_t const * const p_cfg)
{
    wdt_instance_ctrl_t * p_instance_ctrl = (wdt_instance_ctrl_t *) p_ctrl;
    fsp_err_t             err;

    /* Check validity of the parameters */
    err = r_wdt_parameter_checking(p_instance_ctrl, p_cfg);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);

    /* The Cortex-M33's own watchdog. */
    p_instance_ctrl->p_reg = R_WDT0;

    R_BSP_MODULE_START(FSP_IP_WDT, 0);

    /* Underflow/refresh error: reset request (1) or interrupt/NMI request (0). */
    p_instance_ctrl->p_reg->WDT0_WDTRCR_b.RSTIRQS = (uint8_t) p_cfg->reset_control;
    p_instance_ctrl->p_reg->WDT0_WDTCSTPR_b.SLCSTP = (uint8_t) p_cfg->stop_control;

    uint32_t wdtcr = WDT_PRV_WDTCR_SETTING_SET(TIMEOUT, (uint16_t) g_wdtcr_timeout[p_cfg->timeout]);
    wdtcr |= WDT_PRV_WDTCR_SETTING_SET(CLOCK_DIVISION, (uint16_t) p_cfg->clock_division);
    wdtcr |= WDT_PRV_WDTCR_SETTING_SET(WINDOW_START, (uint16_t) p_cfg->window_start);
    wdtcr |= WDT_PRV_WDTCR_SETTING_SET(WINDOW_END, (uint16_t) p_cfg->window_end);

    p_instance_ctrl->p_reg->WDT0_WDTCR = (uint16_t) wdtcr;

    p_instance_ctrl->wdt_open = WDT_OPEN;

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * Refresh the watchdog timer. Implements @ref wdt_api_t::refresh.
 *
 * In addition to refreshing the watchdog counter this function can be used to start the counter in register start mode.
 *
 * @retval FSP_SUCCESS              WDT successfully refreshed.
 * @retval FSP_ERR_ASSERTION        p_ctrl is NULL.
 * @retval FSP_ERR_NOT_OPEN         Instance control block is not initialized.
 *
 * @note This function only returns FSP_SUCCESS. If the refresh fails due to being performed outside of the
 *       permitted refresh period the device will trigger an ISR to run.
 **********************************************************************************************************************/
fsp_err_t R_WDT_Refresh (wdt_ctrl_t * const p_ctrl)
{
    wdt_instance_ctrl_t * p_instance_ctrl = (wdt_instance_ctrl_t *) p_ctrl;

#if WDT_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(WDT_OPEN == p_instance_ctrl->wdt_open, FSP_ERR_NOT_OPEN);
#endif

    p_instance_ctrl->p_reg->WDT0_WDTRR = WDT_PRV_REFRESH_STEP_1;
    p_instance_ctrl->p_reg->WDT0_WDTRR = WDT_PRV_REFRESH_STEP_2;

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * Read the WDT status flags. Implements @ref wdt_api_t::statusGet.
 *
 * Indicates both status and error conditions.
 *
 * @retval FSP_SUCCESS              WDT status successfully read.
 * @retval FSP_ERR_ASSERTION        Null pointer as a parameter.
 * @retval FSP_ERR_NOT_OPEN         Instance control block is not initialized.
 **********************************************************************************************************************/
fsp_err_t R_WDT_StatusGet (wdt_ctrl_t * const p_ctrl, wdt_status_t * const p_status)
{
    wdt_instance_ctrl_t * p_instance_ctrl = (wdt_instance_ctrl_t *) p_ctrl;

#if WDT_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(NULL != p_status);
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(WDT_OPEN == p_instance_ctrl->wdt_open, FSP_ERR_NOT_OPEN);
#endif

    /* Check for refresh or underflow errors. */
    *p_status = (wdt_status_t) (p_instance_ctrl->p_reg->WDT0_WDTSR >> 14);

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * Clear the WDT status and error flags. Implements @ref wdt_api_t::statusClear.
 *
 * @retval FSP_SUCCESS              WDT flag(s) successfully cleared.
 * @retval FSP_ERR_ASSERTION        Null pointer as a parameter.
 * @retval FSP_ERR_NOT_OPEN         Instance control block is not initialized.
 **********************************************************************************************************************/
fsp_err_t R_WDT_StatusClear (wdt_ctrl_t * const p_ctrl, const wdt_status_t status)
{
    wdt_instance_ctrl_t * p_instance_ctrl = (wdt_instance_ctrl_t *) p_ctrl;

    uint16_t value;
    uint16_t read_value;

#if WDT_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(WDT_OPEN == p_instance_ctrl->wdt_open, FSP_ERR_NOT_OPEN);
#endif

    /* Casts to uint16_t to ensure value is handled as unsigned. */
    value = (uint16_t) status;

    /* Write zero to clear flags. */
    value = (uint16_t) ~value;
    value = (uint16_t) (value << 14);

    /* Read back status flags until required flag(s) cleared. */
    /* Flags cannot be cleared until the clock cycle after they are set.  */
    do
    {
        p_instance_ctrl->p_reg->WDT0_WDTSR = value;
        read_value = p_instance_ctrl->p_reg->WDT0_WDTSR;

        /* Isolate flags to clear. */
        read_value &= (uint16_t) ((uint16_t) status << 14);
    } while (0U != read_value);

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * Read the current count value of the WDT. Implements @ref wdt_api_t::counterGet.
 *
 * @retval FSP_SUCCESS          WDT current count successfully read.
 * @retval FSP_ERR_ASSERTION    Null pointer passed as a parameter.
 * @retval FSP_ERR_NOT_OPEN     Instance control block is not initialized.
 **********************************************************************************************************************/
fsp_err_t R_WDT_CounterGet (wdt_ctrl_t * const p_ctrl, uint32_t * const p_count)
{
    wdt_instance_ctrl_t * p_instance_ctrl = (wdt_instance_ctrl_t *) p_ctrl;

#if WDT_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(NULL != p_count);
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(WDT_OPEN == p_instance_ctrl->wdt_open, FSP_ERR_NOT_OPEN);
#endif

    *p_count  = (uint32_t) p_instance_ctrl->p_reg->WDT0_WDTSR;
    *p_count &= WDT_PRV_WDTSR_COUNTER_MASK;

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * @} (end addtogroup WDT)
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Parameter checking function for WDT Open
 *
 * @param[in]    p_instance_ctrl   Pointer to instance control structure
 * @param[in]    p_cfg             Pointer to configuration structure
 *
 * @retval FSP_SUCCESS              WDT successfully configured.
 * @retval FSP_ERR_ASSERTION        Null pointer, or one or more configuration options is invalid.
 * @retval FSP_ERR_ALREADY_OPEN     Module is already open.  This module can only be opened once.
 **********************************************************************************************************************/
static fsp_err_t r_wdt_parameter_checking (wdt_instance_ctrl_t * const p_instance_ctrl, wdt_cfg_t const * const p_cfg)
{
#if WDT_CFG_PARAM_CHECKING_ENABLE

    /* Check that control and config structure pointers are valid. */
    FSP_ASSERT(NULL != p_cfg);
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(WDT_OPEN != p_instance_ctrl->wdt_open, FSP_ERR_ALREADY_OPEN);

    /* Check timeout parameter is supported by WDT. */

    /* Enum checking is done here because some enums in wdt_timeout_t are not supported by the WDT peripheral (they are
     * included for other implementations of the watchdog interface). */
    FSP_ASSERT((p_cfg->timeout == WDT_TIMEOUT_1024) || (p_cfg->timeout == WDT_TIMEOUT_4096) || \
               (p_cfg->timeout == WDT_TIMEOUT_8192) || (p_cfg->timeout == WDT_TIMEOUT_16384));

    /* The WDT0 CKS[3:0] field only encodes /1 /16 /32 /64 /128 /256 (hardware manual 5.4.2.2.2); the
     * other wdt_clock_division_t values (/4 /512 /2048 /8192) are prohibited settings here. */
    FSP_ASSERT((p_cfg->clock_division == WDT_CLOCK_DIVISION_1) ||
               (p_cfg->clock_division == WDT_CLOCK_DIVISION_16) ||
               (p_cfg->clock_division == WDT_CLOCK_DIVISION_32) ||
               (p_cfg->clock_division == WDT_CLOCK_DIVISION_64) ||
               (p_cfg->clock_division == WDT_CLOCK_DIVISION_128) ||
               (p_cfg->clock_division == WDT_CLOCK_DIVISION_256));
#else
    FSP_PARAMETER_NOT_USED(p_instance_ctrl);
    FSP_PARAMETER_NOT_USED(p_cfg);
#endif

    return FSP_SUCCESS;
}
