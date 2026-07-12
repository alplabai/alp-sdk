/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
* Copyright (c) 2026 Alp Lab AB  (RZ/V2N MHU-B port)
*
* SPDX-License-Identifier: BSD-3-Clause
*
* ====== alp-sdk #683: MHU-B port -- register-semantics notes (READ BEFORE TRUSTING THIS ON SILICON) ======
*
* RZ/V2N's MHU is the "MHU-B" IP.  Like the plain MHU r_mhu_ns.c implements, it
* maps each channel to ONE register block computed LINEARLY from R_MHU_NS0_BASE
* (mhu_iodefine.h: R_MHU_NS0..R_MHU_NS41 at 0x20 stride, each the SAME R_MHU0_Type
* layout -- MSG_INT_{STSn,SETn,CLRn} + RESERVED + RSP_INT_{STSn,SETn,CLRn}); only
* 4 channels are valid on this core (BSP_FEATURE_MHU_B_NS_VALID_CHANNEL_MASK =
* 0x00820820 -> {5, 11, 17, 23}, enforced by param-checking).  Channel N uses
* R_MHU_NSN with the two INT halves as the two directions (MSG = this core's RX,
* RSP = this core's TX).
*
* Earlier revisions of this port modelled MHU-B as a 42-slot register CROSSBAR
* (bsp_mhu_b.h's R_BSP_MHU_B_NS_REG_PAIR_BODY, which pairs logical channel 5 to
* {send=R_MHU_NS36, recv=R_MHU_NS8}) and carried two register pointers.  That
* model was WRONG: #697's on-silicon bench (kicking R_MHU_NS5 -- the
* channel-numbered slot -- fired the M33's MHU_MSG5_NS_IRQn(293), while NS8/NS36
* did not) plus the canonical FSP rzv/r_mhu_ns.c:99 both confirm the linear
* mapping.  p_regs_rx is kept == p_regs so the ISR code is unchanged; a channel's
* RX and TX are the two INT halves of the one R_MHU_NSN block.
*
* FLAG FOR REVIEW -- the one piece of this port that is INFERRED, not read
* directly off a vendor rzv2n source (none ships): how send_type (MSG-role
* vs RSP-role, i.e. which of MSG_INT_x / RSP_INT_x this channel asserts on
* send and expects on receive) is derived for MHU-B.  The plain driver reads
* it off BSP_FEATURE_MHU_NS_SEND_TYPE_RSP_VALID_CHANNEL_MASK, a channel bitmask
* that does not exist for MHU-B.  What DOES exist is bsp_mhu_b.h's
* R_BSP_MHU_B_NS_SEND_TYPE_MSG_BODY / _RSP_BODY: two 4-entry IRQn_Type tables,
* one per valid channel, that are clearly a bijective pair (MSG_BODY lists the
* RSP*_NS_IRQn for each channel, RSP_BODY lists the MSG*_NS_IRQn) -- i.e. for a
* MSG-role channel you receive on the RSP IRQ, and vice versa, exactly the
* plain driver's MSG/RSP duality applied to per-channel-dedicated IRQ lines
* instead of one shared IRQ.  This port derives send_type by matching the
* caller's declared rx_irq (mhu_cfg_t::rx_irq, which Zephyr's glue sets from
* the devicetree `interrupts` property) against those two tables -- i.e. the
* config declares which IRQ it expects, and the driver infers which role that
* implies.  This inversion is grounded in the vendor's own bijective tables
* but the INVERSION ITSELF (config-declares-IRQ -> infer role) is this port's
* design choice, not something read off a shipped MHU-B driver.  Likewise,
* which physical register (send vs recv) uses which half (MSG_INT_* vs
* RSP_INT_*) for a given role mirrors the plain driver's if/else structure
* verbatim, just split across two register pointers instead of one --
* reasonable, but unconfirmed against real MHU-B traffic.  Both points need
* bench confirmation before this is trusted beyond "links + matches the
* register layout".
* ============================================================================
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_mhu_b_ns.h"

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/** "MHUB" in ASCII (distinct from plain r_mhu_ns.c's "MHU"), used to determine if channel is open. */
#define MHU_B_NS_OPEN                (0x4D485542ULL)

#define MHU_B_NS_SHMEM_CH_SIZE       (0x8)
#define MHU_B_NS_RSP_TXD_OFFSET      (0x0)
#define MHU_B_NS_MSG_TXD_OFFSET      (0x4)

/** Number of MHU-B logical channels valid on this core (BSP_FEATURE_MHU_B_NS_VALID_CHANNEL_MASK: {5,11,17,23}). */
#define MHU_B_NS_VALID_CHANNEL_COUNT (4U)

/***********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/
#if defined(__ARMCC_VERSION) || defined(__ICCARM__)
typedef void (BSP_CMSE_NONSECURE_CALL * mhu_b_ns_prv_ns_callback)(mhu_callback_args_t * p_args);
#elif defined(__GNUC__)
typedef BSP_CMSE_NONSECURE_CALL void (*volatile mhu_b_ns_prv_ns_callback)(mhu_callback_args_t * p_args);
#endif

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static void r_mhu_b_ns_set_send_data(mhu_b_ns_instance_ctrl_t * p_instance_ctrl, uint32_t msg);

static fsp_err_t r_mhu_b_ns_common_preamble(mhu_b_ns_instance_ctrl_t * p_instance_ctrl);

static fsp_err_t r_mhu_b_ns_send_type_get(uint32_t channel, IRQn_Type rx_irq, mhu_send_type_t * p_send_type);

#if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE
static fsp_err_t r_mhu_b_ns_open_param_checking(mhu_b_ns_instance_ctrl_t * p_instance_ctrl,
                                                mhu_cfg_t const * const   p_cfg);

#endif

/* ISR. */
void mhu_b_ns_int_isr(void);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/

extern uint32_t __mhu_shmem_start;

static const uint32_t g_shmem_base = (uint32_t) &__mhu_shmem_start;

/** The valid MHU-B channels, in the same order bsp_mhu_b.h's SEND_TYPE_*_BODY tables list them. */
static const uint32_t g_mhu_b_ns_valid_channels[MHU_B_NS_VALID_CHANNEL_COUNT] = { 5U, 11U, 17U, 23U };

/** rx_irq to expect when this channel's role/send_type is MSG (bsp_mhu_b.h R_BSP_MHU_B_NS_SEND_TYPE_MSG_BODY --
 * see this file's header note: these are the RSPn_NS_IRQn values, the plain driver's MSG-sends-listens-on-RSP
 * duality). */
static const IRQn_Type g_mhu_b_ns_rx_irq_if_msg[MHU_B_NS_VALID_CHANNEL_COUNT] =
{
    R_BSP_MHU_B_NS_SEND_TYPE_MSG_BODY
};

/** rx_irq to expect when this channel's role/send_type is RSP (bsp_mhu_b.h R_BSP_MHU_B_NS_SEND_TYPE_RSP_BODY --
 * the MSGn_NS_IRQn values). */
static const IRQn_Type g_mhu_b_ns_rx_irq_if_rsp[MHU_B_NS_VALID_CHANNEL_COUNT] =
{
    R_BSP_MHU_B_NS_SEND_TYPE_RSP_BODY
};

/***********************************************************************************************************************
 * Global Variables
 **********************************************************************************************************************/

/** MHU_B_NS Implementation of MHU Driver  */
const mhu_api_t g_mhu_b_ns_on_mhu_b_ns =
{
    .open        = R_MHU_B_NS_Open,
    .msgSend     = R_MHU_B_NS_MsgSend,
    .callbackSet = R_MHU_B_NS_CallbackSet,
    .close       = R_MHU_B_NS_Close,
};

/*******************************************************************************************************************//**
 * @addtogroup MHU_B_NS
 * @{
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Initializes the MHU_B_NS module instance. Implements @ref mhu_api_t::open.
 *
 * @retval FSP_SUCCESS                 Initialization was successful.
 * @retval FSP_ERR_ASSERTION           A required input pointer is NULL.
 * @retval FSP_ERR_ALREADY_OPEN        R_MHU_B_NS_Open has already been called for this p_ctrl.
 * @retval FSP_ERR_INVALID_ARGUMENT    The specified IRQ number is invalid, or does not match either the
 *                                     MSG-role or RSP-role rx_irq bsp_mhu_b.h declares for this channel.
 * @retval FSP_ERR_INVALID_CHANNEL     Requested channel number is not available on MHU-B.
 **********************************************************************************************************************/
fsp_err_t R_MHU_B_NS_Open (mhu_ctrl_t * const p_ctrl, mhu_cfg_t const * const p_cfg)
{
    mhu_b_ns_instance_ctrl_t * p_instance_ctrl = (mhu_b_ns_instance_ctrl_t *) p_ctrl;

#if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE
    fsp_err_t err = r_mhu_b_ns_open_param_checking(p_instance_ctrl, p_cfg);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
#endif

    uint32_t channel = p_cfg->channel;

    /* Resolve the channel's register block by the LINEAR formula the canonical FSP
     * driver uses (hal_renesas rzv/r_mhu_ns/r_mhu_ns.c:99): each R_MHU_NSn slot is
     * one channel, `R_MHU_NS1_BASE - R_MHU_NS0_BASE` (0x20) apart.  For MHU-B only
     * channels {5,11,17,23} are valid (rejected otherwise by param-checking), and
     * each is ONE register block with the two INT halves as the two directions
     * (MSG = A55->M33 / this core's RX, RSP = M33->A55 / this core's TX) -- NOT the
     * separate send/recv slots the older crossbar model invented.  Bench-proven on
     * e1mx-v2n-m1-01: channel 5 -> R_MHU_NS5 (0x504800A0), whose MSG_INT is the one
     * routed to MHU_MSG5_NS_IRQn(293) (#697). */
    p_instance_ctrl->p_regs =
        (R_MHU0_Type *) (R_MHU_NS0_BASE +
                         (channel * ((intptr_t) R_MHU_NS1_BASE - (intptr_t) R_MHU_NS0_BASE)));
    p_instance_ctrl->p_regs_rx = p_instance_ctrl->p_regs;

    /* Derive send_type from the caller's declared rx_irq (see this file's header -- FLAG FOR REVIEW). */
    mhu_send_type_t send_type;
    fsp_err_t       send_type_err = r_mhu_b_ns_send_type_get(channel, p_cfg->rx_irq, &send_type);
    FSP_ERROR_RETURN(FSP_SUCCESS == send_type_err, send_type_err);
    p_instance_ctrl->send_type = send_type;

    if (0 != p_cfg->p_shared_memory)
    {
        /* Use specified address */
        if (p_instance_ctrl->send_type == MHU_SEND_TYPE_RSP)
        {
            p_instance_ctrl->p_shared_memory_tx = (uint32_t *) (((uint32_t) p_cfg->p_shared_memory) +
                                                                MHU_B_NS_RSP_TXD_OFFSET);
            p_instance_ctrl->p_shared_memory_rx = (uint32_t *) (((uint32_t) p_cfg->p_shared_memory) +
                                                                MHU_B_NS_MSG_TXD_OFFSET);
        }
        else
        {
            p_instance_ctrl->p_shared_memory_tx = (uint32_t *) (((uint32_t) p_cfg->p_shared_memory) +
                                                                MHU_B_NS_MSG_TXD_OFFSET);
            p_instance_ctrl->p_shared_memory_rx = (uint32_t *) (((uint32_t) p_cfg->p_shared_memory) +
                                                                MHU_B_NS_RSP_TXD_OFFSET);
        }
    }
    else
    {
        /* Use default location */
        if (p_instance_ctrl->send_type == MHU_SEND_TYPE_RSP)
        {
            p_instance_ctrl->p_shared_memory_tx =
                (uint32_t *) (g_shmem_base + (MHU_B_NS_SHMEM_CH_SIZE * channel) +
                              MHU_B_NS_RSP_TXD_OFFSET);
            p_instance_ctrl->p_shared_memory_rx =
                (uint32_t *) (g_shmem_base + (MHU_B_NS_SHMEM_CH_SIZE * channel) +
                              MHU_B_NS_MSG_TXD_OFFSET);
        }
        else
        {
            p_instance_ctrl->p_shared_memory_tx =
                (uint32_t *) (g_shmem_base + (MHU_B_NS_SHMEM_CH_SIZE * channel) +
                              MHU_B_NS_MSG_TXD_OFFSET);
            p_instance_ctrl->p_shared_memory_rx =
                (uint32_t *) (g_shmem_base + (MHU_B_NS_SHMEM_CH_SIZE * channel) +
                              MHU_B_NS_RSP_TXD_OFFSET);
        }
    }

    /* Power on the MHU-B channel. */
    R_BSP_MODULE_START(FSP_IP_MHU, channel);

    /* Set callback and context pointers */

#if BSP_TZ_SECURE_BUILD

    /* If this is a secure build, the callback provided in p_cfg must be secure. */
    p_instance_ctrl->callback_is_secure = true;
#endif
    p_instance_ctrl->p_callback        = p_cfg->p_callback;
    p_instance_ctrl->p_context         = p_cfg->p_context;
    p_instance_ctrl->p_callback_memory = NULL;

    p_instance_ctrl->p_cfg   = p_cfg;
    p_instance_ctrl->channel = channel;
    p_instance_ctrl->open    = MHU_B_NS_OPEN;

    /* Enable the RX IRQ LAST, only after the control block is fully
     * initialised.  If the peer (e.g. an already-booted A55) has already
     * armed the doorbell, the ISR can fire the instant the NVIC line is
     * enabled -- enabling it before p_callback/channel are set would run the
     * ISR against a NULL callback and channel 0, silently dropping the first
     * message. */
    R_BSP_IrqCfgEnable(p_cfg->rx_irq, p_cfg->rx_ipl, p_instance_ctrl);

    /* All done.  */
    return FSP_SUCCESS;
}

/**********************************************************************************************************************
 * End of function R_MHU_B_NS_Open
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Send message via MHU-B.
 * Implements @ref mhu_api_t::msgSend.
 *
 * @retval FSP_SUCCESS                 Send message successfully.
 * @retval FSP_ERR_ASSERTION           A required pointer was NULL.
 * @retval FSP_ERR_NOT_OPEN            The instance control structure is not opened.
 **********************************************************************************************************************/
fsp_err_t R_MHU_B_NS_MsgSend (mhu_ctrl_t * const p_ctrl, uint32_t const msg)
{
    mhu_b_ns_instance_ctrl_t * p_instance_ctrl = (mhu_b_ns_instance_ctrl_t *) p_ctrl;

    fsp_err_t err = r_mhu_b_ns_common_preamble(p_instance_ctrl);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);

    /* Set msg. */
    r_mhu_b_ns_set_send_data(p_instance_ctrl, msg);

    return FSP_SUCCESS;
}

/**********************************************************************************************************************
 * End of function R_MHU_B_NS_MsgSend
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Updates the user callback with the option to provide memory for the callback argument structure.
 * Implements @ref mhu_api_t::callbackSet.
 *
 * @retval  FSP_SUCCESS                  Callback updated successfully.
 * @retval  FSP_ERR_ASSERTION            A required pointer is NULL.
 * @retval  FSP_ERR_NOT_OPEN             The control block has not been opened.
 * @retval  FSP_ERR_NO_CALLBACK_MEMORY   p_callback is non-secure and p_callback_memory is either secure or NULL.
 **********************************************************************************************************************/
fsp_err_t R_MHU_B_NS_CallbackSet (mhu_ctrl_t * const          p_api_ctrl,
                                  void (                    * p_callback)(mhu_callback_args_t *),
                                  void const * const          p_context,
                                  mhu_callback_args_t * const p_callback_memory)
{
    mhu_b_ns_instance_ctrl_t * p_ctrl = (mhu_b_ns_instance_ctrl_t *) p_api_ctrl;

#if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(p_ctrl);
    FSP_ASSERT(p_callback);
    FSP_ERROR_RETURN(MHU_B_NS_OPEN == p_ctrl->open, FSP_ERR_NOT_OPEN);
#endif

#if BSP_TZ_SECURE_BUILD

    /* Get security state of p_callback */
    p_ctrl->callback_is_secure =
        (NULL == cmse_check_address_range((void *) p_callback, sizeof(void *), CMSE_AU_NONSECURE));

 #if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE

    /* In secure projects, p_callback_memory must be provided in non-secure space if p_callback is non-secure */
    mhu_callback_args_t * const p_callback_memory_checked = cmse_check_pointed_object(p_callback_memory,
                                                                                       CMSE_AU_NONSECURE);
    FSP_ERROR_RETURN(p_ctrl->callback_is_secure || (NULL != p_callback_memory_checked), FSP_ERR_NO_CALLBACK_MEMORY);
 #endif
#endif

    /* Store callback and context */

#if BSP_TZ_SECURE_BUILD

    /* cmse_check_address_range returns NULL if p_callback is located in secure memory */
    p_ctrl->callback_is_secure =
        (NULL == cmse_check_address_range((void *) p_callback, sizeof(void *), CMSE_AU_NONSECURE));
#endif
    p_ctrl->p_callback        = p_callback;
    p_ctrl->p_context         = p_context;
    p_ctrl->p_callback_memory = p_callback_memory;

    return FSP_SUCCESS;
}

/**********************************************************************************************************************
 * End of function R_MHU_B_NS_CallbackSet
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Disables interrupts, clears internal driver data.
 * @ref mhu_api_t::close.
 *
 * @retval FSP_SUCCESS                 MHU_B_NS closed.
 * @retval FSP_ERR_ASSERTION           p_ctrl is NULL.
 * @retval FSP_ERR_NOT_OPEN            The instance control structure is not opened.
 **********************************************************************************************************************/
fsp_err_t R_MHU_B_NS_Close (mhu_ctrl_t * const p_ctrl)
{
    mhu_b_ns_instance_ctrl_t * p_instance_ctrl = (mhu_b_ns_instance_ctrl_t *) p_ctrl;

    fsp_err_t err = r_mhu_b_ns_common_preamble(p_instance_ctrl);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);

    /* Cleanup the device: disable interrupts */

    NVIC_DisableIRQ(p_instance_ctrl->p_cfg->rx_irq);
    R_FSP_IsrContextSet(p_instance_ctrl->p_cfg->rx_irq, p_instance_ctrl);

    p_instance_ctrl->open = 0U;

    return FSP_SUCCESS;
}

/**********************************************************************************************************************
 * End of function R_MHU_B_NS_Close
 *********************************************************************************************************************/

/** @} (end addtogroup MHU_B_NS) */

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/

#if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE

/*******************************************************************************************************************//**
 * Parameter checking for R_MHU_B_NS_Open.
 *
 * @param[in] p_instance_ctrl          Pointer to instance control structure.
 * @param[in]  p_cfg              Configuration structure for this instance
 *
 * @retval FSP_SUCCESS                 Initialization was successful.
 * @retval FSP_ERR_ASSERTION           A required input pointer is NULL.
 * @retval FSP_ERR_ALREADY_OPEN        R_MHU_B_NS_Open has already been called for this p_ctrl.
 * @retval FSP_ERR_INVALID_ARGUMENT    The specified IRQ number is invalid.
 * @retval FSP_ERR_INVALID_CHANNEL     Requested channel number is not available on MHU-B.
 **********************************************************************************************************************/
static fsp_err_t r_mhu_b_ns_open_param_checking (mhu_b_ns_instance_ctrl_t * p_instance_ctrl,
                                                 mhu_cfg_t const * const   p_cfg)
{
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ASSERT(NULL != p_cfg);
    FSP_ERROR_RETURN(MHU_B_NS_OPEN != p_instance_ctrl->open, FSP_ERR_ALREADY_OPEN);

    /* Validate channel number against the MHU-B (not plain-MHU) valid-channel mask. */
    FSP_ERROR_RETURN(((1U << p_cfg->channel) & BSP_FEATURE_MHU_B_NS_VALID_CHANNEL_MASK), FSP_ERR_INVALID_CHANNEL);

    FSP_ERROR_RETURN(FSP_INVALID_VECTOR != p_cfg->rx_irq, FSP_ERR_INVALID_ARGUMENT);

    return FSP_SUCCESS;
}

#endif

/*******************************************************************************************************************//**
 * Common code at the beginning of all MHU_B_NS functions except open.
 *
 * @param[in] p_instance_ctrl          Pointer to instance control structure.
 *
 * @retval FSP_SUCCESS                 No invalid conditions detected, MHU_B_NS state matches expected state.
 * @retval FSP_ERR_ASSERTION           p_ctrl is null.
 * @retval FSP_ERR_NOT_OPEN            The instance control structure is not opened.
 **********************************************************************************************************************/
static fsp_err_t r_mhu_b_ns_common_preamble (mhu_b_ns_instance_ctrl_t * p_instance_ctrl)
{
#if MHU_B_NS_CFG_PARAM_CHECKING_ENABLE
    FSP_ASSERT(NULL != p_instance_ctrl);
    FSP_ERROR_RETURN(MHU_B_NS_OPEN == p_instance_ctrl->open, FSP_ERR_NOT_OPEN);
#else
    FSP_PARAMETER_NOT_USED(p_instance_ctrl);
#endif

    return FSP_SUCCESS;
}

/**********************************************************************************************************************
 * End of function r_mhu_b_ns_common_preamble
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Derive send_type (MSG-role vs RSP-role) for a valid MHU-B channel from the caller's declared rx_irq.
 * See this file's header -- FLAG FOR REVIEW, this derivation is inferred, not read off a vendor source.
 *
 * @param[in]  channel      FSP/hardware channel (already known to be one of {5, 11, 17, 23}).
 * @param[in]  rx_irq       The IRQ the caller configured to receive on.
 * @param[out] p_send_type  Resolved send_type.
 *
 * @retval FSP_SUCCESS                 rx_irq matched one of bsp_mhu_b.h's per-channel MSG/RSP IRQ tables.
 * @retval FSP_ERR_INVALID_ARGUMENT    rx_irq matched neither table for this channel.
 **********************************************************************************************************************/
static fsp_err_t r_mhu_b_ns_send_type_get (uint32_t channel, IRQn_Type rx_irq, mhu_send_type_t * p_send_type)
{
    for (uint32_t i = 0; i < MHU_B_NS_VALID_CHANNEL_COUNT; i++)
    {
        if (g_mhu_b_ns_valid_channels[i] != channel)
        {
            continue;
        }

        if (rx_irq == g_mhu_b_ns_rx_irq_if_msg[i])
        {
            *p_send_type = MHU_SEND_TYPE_MSG;

            return FSP_SUCCESS;
        }

        if (rx_irq == g_mhu_b_ns_rx_irq_if_rsp[i])
        {
            *p_send_type = MHU_SEND_TYPE_RSP;

            return FSP_SUCCESS;
        }

        break;
    }

    return FSP_ERR_INVALID_ARGUMENT;
}

/**********************************************************************************************************************
 * End of function r_mhu_b_ns_send_type_get
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Write a message to shared memory and generate inter-core interrupt on the SEND register.
 *
 * @param[in]  p_instance_ctrl    Control block for this instance
 * @param[in]  msg                32bit send data
 **********************************************************************************************************************/
static void r_mhu_b_ns_set_send_data (mhu_b_ns_instance_ctrl_t * p_instance_ctrl, uint32_t msg)
{
    if (MHU_SEND_TYPE_MSG == p_instance_ctrl->send_type)
    {
        /* Check interrupt status: Has the previous message been received? */
        do
        {
            /* Do Nothing */
        } while (0 != p_instance_ctrl->p_regs->MSG_INT_STSn);

        /* Store the message data. */
        *p_instance_ctrl->p_shared_memory_tx = msg;

        /* Assert interrupt. */
        p_instance_ctrl->p_regs->MSG_INT_SETn = 1;
    }
    else
    {
        /* Check interrupt status: Has the previous message been received? */
        do
        {
            /* Do Nothing */
        } while (0 != p_instance_ctrl->p_regs->RSP_INT_STSn);

        /* Store the message data. */
        *p_instance_ctrl->p_shared_memory_tx = msg;

        /* Assert interrupt. */
        p_instance_ctrl->p_regs->RSP_INT_SETn = 1;
    }
}

/**********************************************************************************************************************
 * End of function r_mhu_b_ns_set_send_data
 *********************************************************************************************************************/

/*********************************************************************************************************************
 * MHU_B_NS receive interrupt (called by the Zephyr glue's IRQ_CONNECT target).
 **********************************************************************************************************************/
void mhu_b_ns_int_isr (void)
{
    /* Save context if RTOS is used */
    FSP_CONTEXT_SAVE

    IRQn_Type irq = R_FSP_CurrentIrqGet();

    R_MHU_B_NS_IsrSub(irq);

    /* Restore context if RTOS is used */
    FSP_CONTEXT_RESTORE
}

/**********************************************************************************************************************
 * End of function mhu_b_ns_int_isr
 *********************************************************************************************************************/

/*******************************************************************************************************************//**
 * MHU_B_NS receive interrupt sub function.  Services the RECEIVE register (p_regs_rx), not the send
 * register the ISR fired through addressing -- see this file's header.
 *
 * @param[in]  irq    irq number for inter-core interrupt
 **********************************************************************************************************************/
void R_MHU_B_NS_IsrSub (uint32_t irq)
{
    uint32_t msg;

    /* Clear pending IRQ to make sure it doesn't fire again after exiting */
    R_BSP_IrqStatusClear(irq);

    /* Recover ISR context saved in open. */
    mhu_b_ns_instance_ctrl_t * p_instance_ctrl = (mhu_b_ns_instance_ctrl_t *) R_FSP_IsrContextGet(irq);

    /* Check interrupt reason on the RECEIVE register. */
    if (
        ((MHU_SEND_TYPE_RSP == p_instance_ctrl->send_type) && (0 != p_instance_ctrl->p_regs_rx->MSG_INT_STSn)) ||
        ((MHU_SEND_TYPE_MSG == p_instance_ctrl->send_type) && (0 != p_instance_ctrl->p_regs_rx->RSP_INT_STSn)))
    {
        /* Read data */
        msg = *p_instance_ctrl->p_shared_memory_rx;

        /* Clear interrupt on the RECEIVE register. */
        if (MHU_SEND_TYPE_RSP == p_instance_ctrl->send_type)
        {
            p_instance_ctrl->p_regs_rx->MSG_INT_CLRn = 1;
        }
        else
        {
            p_instance_ctrl->p_regs_rx->RSP_INT_CLRn = 1;
        }

        /* Invoke the callback function if it is set. */
        if (NULL != p_instance_ctrl->p_callback)
        {
            /* Setup parameters for the user-supplied callback function. */
            mhu_callback_args_t callback_args;

            /* Store callback arguments in memory provided by user if available.  This allows callback arguments to be
             * stored in non-secure memory so they can be accessed by a non-secure callback function. */
            mhu_callback_args_t * p_args = p_instance_ctrl->p_callback_memory;
            if (NULL == p_args)
            {
                /* Store on stack */
                p_args = &callback_args;
            }
            else
            {
                /* Save current arguments on the stack in case this is a nested interrupt. */
                callback_args = *p_args;
            }

            p_args->p_context = p_instance_ctrl->p_context;

            p_args->channel = p_instance_ctrl->channel;
            p_args->msg     = msg;

#if BSP_TZ_SECURE_BUILD

            /* p_callback can point to a secure function or a non-secure function. */
            if (p_instance_ctrl->callback_is_secure)
            {
                /* If p_callback is secure, then the project does not need to change security state. */
                p_instance_ctrl->p_callback(p_args);
            }
            else
            {
                /* If p_callback is Non-secure, then the project must change to Non-secure state
                 * in order to call the callback. */
                mhu_b_ns_prv_ns_callback p_callback = (mhu_b_ns_prv_ns_callback) (p_instance_ctrl->p_callback);
                p_callback(p_args);
            }

#else

            /* If the project is not Trustzone Secure, then it will never need to change security state
             * in order to call the callback. */
            p_instance_ctrl->p_callback(p_args);
#endif

            if (NULL != p_instance_ctrl->p_callback_memory)
            {
                /* Restore callback memory in case this is a nested interrupt. */
                *p_instance_ctrl->p_callback_memory = callback_args;
            }
        }
    }
}

/**********************************************************************************************************************
 * End of function R_MHU_B_NS_IsrSub
 *********************************************************************************************************************/
