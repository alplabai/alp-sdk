/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N MHU-B port)
*
* SPDX-License-Identifier: BSD-3-Clause
*
* ====== alp-sdk #683: MHU-B port (structural adaptation, not a verbatim vendor file) ======
* The rzv FSP HAL ships r_mhu_ns for the LINEAR "plain" MHU IP (rzv2l/g2l/g3s):
* one register block per channel at R_MHU_NS0_BASE + channel * stride,
* validated against BSP_FEATURE_MHU_NS_VALID_CHANNEL_MASK and
* BSP_FEATURE_MHU_NS_SEND_TYPE_RSP_VALID_CHANNEL_MASK.  RZ/V2N's MHU is the
* newer "MHU-B" IP: a non-linear 42-slot register crossbar
* (bsp_mhu_b.h R_BSP_MHU_B_NS_REG_PAIR_BODY) where only channels
* {5, 11, 17, 23} are valid (BSP_FEATURE_MHU_B_NS_VALID_CHANNEL_MASK) and
* NEITHER plain-MHU macro above is defined for this SoC -- r_mhu_ns.c does
* not compile for rzv2n.  Renesas has not shipped an r_mhu_b_ns module for
* any FSP-supported part as of this port, so this header/source pair is a
* from-scratch adaptation: same mhu_api_t surface and the same ctrl-struct
* field names the Zephyr glue (mbox_renesas_rz_mhu_b.c, this SDK) reads
* (`p_regs`, `send_type`), but registers are resolved via the pair-body
* table instead of the linear formula.  See r_mhu_b_ns.c's file header for
* the TX/RX register split and the send_type derivation this port infers
* (flagged there for review -- no vendor rzv2n source exists to confirm it).
* ============================================================================
*/

/*******************************************************************************************************************//**
 * @addtogroup MHU_B_NS
 * @{
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_mhu_api.h"
#include "r_mhu_b_ns_cfg.h"

#ifndef R_MHU_B_NS_H
 #define R_MHU_B_NS_H

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/*************************************************************************************************
 * Type defines
 *************************************************************************************************/

/** Channel control block. DO NOT INITIALIZE.  Initialization occurs when @ref mhu_api_t::open is called. */
typedef struct st_mhu_b_ns_instance_ctrl
{
    uint32_t          open;              ///< Indicates whether the open() API has been successfully called.
    mhu_cfg_t const * p_cfg;              ///< Pointer to instance configuration

    /* MHU-B's crossbar sends and receives through DIFFERENT physical register
     * blocks for the asymmetric channels (5, 11, 17) -- unlike the plain MHU,
     * where one register block serves both directions via its MSG_INT_x /
     * RSP_INT_x halves.  p_regs is the SEND register (msgSend()'s SETn write
     * and the pre-send busy poll; also what the Zephyr glue's busy-check
     * reads) -- kept named `p_regs`, matching r_mhu_ns.h's field name, so
     * code written against that layout still lines up.  p_regs_rx is the
     * RECEIVE register the ISR services. */
    R_MHU0_Type * p_regs;                 ///< Send-side register block for this channel.
    R_MHU0_Type * p_regs_rx;              ///< Receive-side register block for this channel.

    uint32_t        channel;              ///< FSP/hardware channel (5, 11, 17 or 23 on rzv2n).
    mhu_send_type_t send_type;             ///< Send Type: Message or Response
    uint32_t      * p_shared_memory_tx;   ///< Pointer to send data area
    uint32_t      * p_shared_memory_rx;   ///< Pointer to recv data area

 #if BSP_TZ_SECURE_BUILD
    bool callback_is_secure;              ///< p_callback is in secure memory
 #endif

    /* Pointer to callback and optional working memory */
    void (* p_callback)(mhu_callback_args_t *);

    /* Pointer to non-secure memory that can be used to pass arguments to a callback in non-secure memory. */
    mhu_callback_args_t * p_callback_memory;

    /* Pointer to context to be passed into callback function */
    void const * p_context;
} mhu_b_ns_instance_ctrl_t;

/** R_MHU_B_NS extended configuration.  Unlike r_mhu_ns_extended_cfg_t (whose
 * p_reg the Zephyr rzg glue populates from a DT `reg` property), this port
 * resolves both the send and receive register blocks itself from the
 * pair-body table by channel number -- p_extend/p_reg is not read.  The type
 * is kept only so an mhu_cfg_t built the same way as an r_mhu_ns caller's
 * (non-NULL p_extend) still compiles. */
typedef struct st_mhu_b_ns_extended_cfg
{
    void * p_reg;                         ///< Unused by this port; see above.
} mhu_b_ns_extended_cfg_t;

/**********************************************************************************************************************
 * Exported global variables
 **********************************************************************************************************************/

/** @cond INC_HEADER_DEFS_SEC */
/** Filled in Interface API structure for this Instance. */
extern const mhu_api_t g_mhu_b_ns_on_mhu_b_ns;

/** @endcond */

/***********************************************************************************************************************
 * Public APIs
 **********************************************************************************************************************/
fsp_err_t R_MHU_B_NS_Open(mhu_ctrl_t * p_ctrl, mhu_cfg_t const * const p_cfg);

fsp_err_t R_MHU_B_NS_MsgSend(mhu_ctrl_t * const p_ctrl, uint32_t const msg);

fsp_err_t R_MHU_B_NS_Close(mhu_ctrl_t * const p_ctrl);

fsp_err_t R_MHU_B_NS_CallbackSet(mhu_ctrl_t * const          p_api_ctrl,
                                 void (                    * p_callback)(mhu_callback_args_t *),
                                 void const * const          p_context,
                                 mhu_callback_args_t * const p_callback_memory);

void R_MHU_B_NS_IsrSub(uint32_t irq);

/** Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif                                 /* R_MHU_B_NS_H */

/*******************************************************************************************************************//**
 * @} (end defgroup MHU_B_NS)
 **********************************************************************************************************************/
