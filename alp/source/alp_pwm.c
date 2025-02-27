/**
 ********************************************************************************
 * @file    alp_adc.c
 * @author  Huseyin ERTURK
 * @date    12/02/2025
 * @brief   Alp ADC Driver implementation
 ********************************************************************************
 

/************************************
 * INCLUDES
 ************************************/
#include "alp_pwm.h"
/************************************
 * EXTERN VARIABLES
 ************************************/

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
// Define any private macros or constants if needed

/************************************
 * PRIVATE TYPEDEFS
 ************************************/
// Define private typedefs if needed

/************************************
 * STATIC VARIABLES
 ************************************/

/************************************
 * GLOBAL VARIABLES
 ************************************/
// Declare any global variables if necessary

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
// Declare static (private) function prototypes if needed

/************************************
 * STATIC FUNCTIONS
 ************************************/
// Implement static (private) functions if needed
/**
 * @fn      void ALP_UTIMER_Interrupt_Enable (ALP_UTIMER_RESOURCES *UTIMER_RES, uint8_t channel)
 * @brief   Enable interrupt for ALP_UTIMER.
 * @note    none.
 * @param   UTIMER_RES : Pointer to utimer resources structure.
 * @param   channel    : Pointer to user callback function.
 * @retval  none
 */
static void ALP_UTIMER_Interrupt_Enable (ALP_UTIMER_RESOURCES *UTIMER_RES, uint8_t channel)
{
    switch (UTIMER_RES->ch_info[channel].channel_counter_dir_backup)
    {
        case ALP_UTIMER_COUNTER_UP:
            if (UTIMER_RES->ch_info[channel].ch_config.utimer_mode)
            {
                utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_OVER_FLOW);
                NVIC_ClearPendingIRQ (UTIMER_OVERFLOW_IRQ(channel));
                NVIC_SetPriority (UTIMER_OVERFLOW_IRQ(channel), UTIMER_RES->ch_info[channel].over_flow_irq_priority);
                NVIC_EnableIRQ (UTIMER_OVERFLOW_IRQ(channel));
            }
            break;

        case ALP_UTIMER_COUNTER_DOWN:
            if (UTIMER_RES->ch_info[channel].ch_config.utimer_mode)
            {
                utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_UNDER_FLOW);
                NVIC_ClearPendingIRQ (UTIMER_UNDERFLOW_IRQ(channel));
                NVIC_SetPriority (UTIMER_UNDERFLOW_IRQ(channel), UTIMER_RES->ch_info[channel].under_flow_irq_priority);
                NVIC_EnableIRQ (UTIMER_UNDERFLOW_IRQ(channel));
            }
            break;

        case ALP_UTIMER_COUNTER_TRIANGLE:
            if (UTIMER_RES->ch_info[channel].ch_config.utimer_mode)
            {
                utimer_unmask_interrupt(UTIMER_RES->regs, channel, (CHAN_INTERRUPT_OVER_FLOW | CHAN_INTERRUPT_UNDER_FLOW));
                NVIC_ClearPendingIRQ (UTIMER_OVERFLOW_IRQ(channel));
                NVIC_SetPriority (UTIMER_OVERFLOW_IRQ(channel), UTIMER_RES->ch_info[channel].over_flow_irq_priority);
                NVIC_EnableIRQ (UTIMER_OVERFLOW_IRQ(channel));
                NVIC_ClearPendingIRQ (UTIMER_UNDERFLOW_IRQ(channel));
                NVIC_SetPriority (UTIMER_UNDERFLOW_IRQ(channel), UTIMER_RES->ch_info[channel].under_flow_irq_priority);
                NVIC_EnableIRQ (UTIMER_UNDERFLOW_IRQ(channel));
            }
            break;
    }

    switch (UTIMER_RES->ch_info[channel].channel_mode_backup)
    {
        case ALP_UTIMER_MODE_CAPTURING:
            if (UTIMER_RES->ch_info[channel].ch_config.driver_A)
            {
                utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_CAPTURE_A);
                NVIC_ClearPendingIRQ (UTIMER_CAPTURE_A_IRQ(channel));
                NVIC_SetPriority (UTIMER_CAPTURE_A_IRQ(channel), UTIMER_RES->ch_info[channel].capture_A_irq_priority);
                NVIC_EnableIRQ (UTIMER_CAPTURE_A_IRQ(channel));
            }
            if (UTIMER_RES->ch_info[channel].ch_config.driver_B)
            {
                utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_CAPTURE_B);
                NVIC_ClearPendingIRQ (UTIMER_CAPTURE_B_IRQ(channel));
                NVIC_SetPriority (UTIMER_CAPTURE_B_IRQ(channel), UTIMER_RES->ch_info[channel].capture_B_irq_priority);
                NVIC_EnableIRQ (UTIMER_CAPTURE_B_IRQ(channel));
            }
            break;

        case ALP_UTIMER_MODE_COMPARING:
            if (UTIMER_RES->ch_info[channel].ch_config.utimer_mode)
            {
                if (UTIMER_RES->ch_info[channel].ch_config.driver_A)
                {
                    utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_CAPTURE_A);
                    NVIC_ClearPendingIRQ (UTIMER_CAPTURE_A_IRQ(channel));
                    NVIC_SetPriority (UTIMER_CAPTURE_A_IRQ(channel), UTIMER_RES->ch_info[channel].capture_A_irq_priority);
                    NVIC_EnableIRQ (UTIMER_CAPTURE_A_IRQ(channel));
                }
                if (UTIMER_RES->ch_info[channel].ch_config.driver_B)
                {
                    utimer_unmask_interrupt(UTIMER_RES->regs, channel, CHAN_INTERRUPT_CAPTURE_B);
                    NVIC_ClearPendingIRQ (UTIMER_CAPTURE_B_IRQ(channel));
                    NVIC_SetPriority (UTIMER_CAPTURE_B_IRQ(channel), UTIMER_RES->ch_info[channel].capture_B_irq_priority);
                    NVIC_EnableIRQ (UTIMER_CAPTURE_B_IRQ(channel));
                }
            }
            break;

        case ALP_UTIMER_MODE_BASIC:
        case ALP_UTIMER_MODE_BUFFERING:
        case ALP_UTIMER_MODE_TRIGGERING:
        case ALP_UTIMER_MODE_DEAD_TIME:
            break;
    }
}
/************************************
 * GLOBAL FUNCTIONS
 ************************************/
