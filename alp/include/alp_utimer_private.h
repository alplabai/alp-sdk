/**
 ********************************************************************************
 * @file    alp_utimer_private.h
 * @author  Huseyin ERTURK
 * @date    12/02/2025
 * @brief   Header file for ALP UTIMER driver
 ********************************************************************************
 */

#ifdef __cplusplus
extern "C" {
#endif
/************************************
 * INCLUDES
 ************************************/
#include "alp_driver_common.h"
#include "stdint.h"
#include "Driver_UTIMER.h"
#include "utimer.h"
/************************************
 * MACROS AND DEFINES
 ************************************/
// TODO: change them into ALP defines.
#define ALP_UTIMER_MAX_CHANNEL                      ARM_UTIMER_MAX_CHANNEL
#define ALP_UTIMER_TOTAL_CHANNELS                   ARM_UTIMER_TOTAL_CHANNELS

#define ALP_UTIMER_MODE_ENABLE                      UTIMER_MODE_ENABLE
#define ALP_QEC_MODE_ENABLE                         QEC_MODE_ENABLE

#define ALP_CHAN_INTERRUPT_CAPTURE_A                CHAN_INTERRUPT_CAPTURE_A
#define ALP_CHAN_INTERRUPT_CAPTURE_B                CHAN_INTERRUPT_CAPTURE_B
#define ALP_CHAN_INTERRUPT_COMPARE_A_BUF1           CHAN_INTERRUPT_COMPARE_A_BUF1
#define ALP_CHAN_INTERRUPT_COMPARE_A_BUF2           CHAN_INTERRUPT_COMPARE_A_BUF2
#define ALP_CHAN_INTERRUPT_COMPARE_B_BUF1           CHAN_INTERRUPT_COMPARE_B_BUF1
#define ALP_CHAN_INTERRUPT_COMPARE_B_BUF2           CHAN_INTERRUPT_COMPARE_B_BUF2
#define ALP_CHAN_INTERRUPT_UNDER_FLOW               CHAN_INTERRUPT_UNDER_FLOW
#define ALP_CHAN_INTERRUPT_OVER_FLOW                CHAN_INTERRUPT_OVER_FLOW

#define ALP_UTIMER_CAPTURE_A_IRQ_BASE               UTIMER_CAPTURE_A_IRQ_BASE
#define ALP_UTIMER_CAPTURE_B_IRQ_BASE               UTIMER_CAPTURE_B_IRQ_BASE
#define ALP_UTIMER_CAPTURE_C_IRQ_BASE               UTIMER_CAPTURE_C_IRQ_BASE
#define ALP_UTIMER_CAPTURE_D_IRQ_BASE               UTIMER_CAPTURE_D_IRQ_BASE
#define ALP_UTIMER_CAPTURE_E_IRQ_BASE               UTIMER_CAPTURE_E_IRQ_BASE
#define ALP_UTIMER_CAPTURE_F_IRQ_BASE               UTIMER_CAPTURE_F_IRQ_BASE
#define ALP_UTIMER_UNDERFLOW_IRQ_BASE               UTIMER_UNDERFLOW_IRQ_BASE
#define ALP_UTIMER_OVERFLOW_IRQ_BASE                UTIMER_OVERFLOW_IRQ_BASE

#define ALP_UTIMER_CAPTURE_A_IRQ(channel)           UTIMER_CAPTURE_A_IRQ(channel)
#define ALP_UTIMER_CAPTURE_B_IRQ(channel)           UTIMER_CAPTURE_B_IRQ(channel)
#define ALP_UTIMER_CAPTURE_C_IRQ(channel)           UTIMER_CAPTURE_C_IRQ(channel)
#define ALP_UTIMER_CAPTURE_D_IRQ(channel)           UTIMER_CAPTURE_D_IRQ(channel)
#define ALP_UTIMER_CAPTURE_E_IRQ(channel)           UTIMER_CAPTURE_E_IRQ(channel)
#define ALP_UTIMER_CAPTURE_F_IRQ(channel)           UTIMER_CAPTURE_F_IRQ(channel)
#define ALP_UTIMER_UNDERFLOW_IRQ(channel)           UTIMER_UNDERFLOW_IRQ(channel)
#define ALP_UTIMER_OVERFLOW_IRQ(channel)            UTIMER_OVERFLOW_IRQ(channel)

#define ALP_QEC_CAPTURE_A_IRQ_BASE                  QEC_CAPTURE_A_IRQ_BASE
#define ALP_QEC_CAPTURE_B_IRQ_BASE                  QEC_CAPTURE_B_IRQ_BASE

#define ALP_QEC_CAPTURE_A_IRQ(channel)              QEC_CAPTURE_A_IRQ(channel)
#define ALP_QEC_CAPTURE_B_IRQ(channel)              QEC_CAPTURE_B_IRQ(channel)


/************************************
 * TYPEDEFS
 ************************************/
/** \brief ALP_UTIMER driver state. */
typedef struct _ALP_UTIMER_DRV_STATE {
    uint32_t initialized : 1;
    uint32_t powered     : 1;
    uint32_t configured  : 1;
    uint32_t triggered   : 1;
    uint32_t started     : 1;
    uint32_t reserved    : 27;
} ALP_UTIMER_DRV_STATE;

/** \brief ALP_UTIMER channel specific configurations. */
typedef struct _ALP_UTIMER_CHANNEL_INFO
{
    utimer_channel_config      ch_config;
    bool                       dc_enable;
    uint8_t                    capture_A_irq_priority;
    uint8_t                    capture_B_irq_priority;
    uint8_t                    capture_C_irq_priority;
    uint8_t                    capture_D_irq_priority;
    uint8_t                    capture_E_irq_priority;
    uint8_t                    capture_F_irq_priority;
    uint8_t                    over_flow_irq_priority;
    uint8_t                    under_flow_irq_priority;
    ALP_UTIMER_MODE            channel_mode_backup;
    ALP_UTIMER_COUNTER_DIR     channel_counter_dir_backup;
    ALP_UTIMER_DRV_STATE       state;
    ALP_UTIMER_SignalEvent_t   CB_function_ptr;
} ALP_UTIMER_CHANNEL_INFO;

/** \brief ALP_UTIMER resource. */
typedef struct _ALP_UTIMER_RESOURCES
{
    UTIMER_Type *regs;
    ALP_UTIMER_CHANNEL_INFO ch_info[ALP_UTIMER_TOTAL_CHANNELS];
} ALP_UTIMER_RESOURCES;


/************************************
 * EXPORTED VARIABLES
 ************************************/
static inline uint32_t ALP_UTIMER_Get_TriggerType (ALP_UTIMER_TRIGGER trigger)
{
    uint32_t value;
    switch (trigger)
    {
        case ALP_UTIMER_SRC0_TRIG0_RISING:
            value = CNTR_SRC0_TRIG0_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG0_FALLING:
            value = CNTR_SRC0_TRIG0_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG1_RISING:
            value = CNTR_SRC0_TRIG1_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG1_FALLING:
            value = CNTR_SRC0_TRIG1_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG2_RISING:
            value = CNTR_SRC0_TRIG2_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG2_FALLING:
            value = CNTR_SRC0_TRIG2_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG3_RISING:
            value = CNTR_SRC0_TRIG3_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG3_FALLING:
            value = CNTR_SRC0_TRIG3_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG4_RISING:
            value = CNTR_SRC0_TRIG4_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG4_FALLING:
            value = CNTR_SRC0_TRIG4_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG5_RISING:
            value = CNTR_SRC0_TRIG5_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG5_FALLING:
            value = CNTR_SRC0_TRIG5_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG6_RISING:
            value = CNTR_SRC0_TRIG6_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG6_FALLING:
            value = CNTR_SRC0_TRIG6_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG7_RISING:
            value = CNTR_SRC0_TRIG7_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG7_FALLING:
            value = CNTR_SRC0_TRIG7_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG8_RISING:
            value = CNTR_SRC0_TRIG8_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG8_FALLING:
            value = CNTR_SRC0_TRIG8_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG9_RISING:
            value = CNTR_SRC0_TRIG9_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG9_FALLING:
            value = CNTR_SRC0_TRIG9_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG10_RISING:
            value = CNTR_SRC0_TRIG10_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG10_FALLING:
            value = CNTR_SRC0_TRIG10_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG11_RISING:
            value = CNTR_SRC0_TRIG11_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG11_FALLING:
            value = CNTR_SRC0_TRIG11_FALLING;
            break;
        case ALP_UTIMER_SRC0_TRIG12_RISING:
            value = CNTR_SRC0_TRIG12_RISING;
            break;
        case ALP_UTIMER_SRC0_TRIG12_FALLING:
            value = CNTR_SRC0_TRIG12_FALLING;
            break;
        default:
            value = 0;
            break;
    }
    return value;
}
/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/



#ifdef __cplusplus
}
#endif

#endif /* ALP_DRIVER_TEMPLATE_H */