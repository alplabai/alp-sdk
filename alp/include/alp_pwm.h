/**
 ********************************************************************************
 * @file    alp_adc.c
 * @author  Huseyin ERTURK
 * @date    12/02/2025
 * @brief   Header file for ALP ADC driver
 ********************************************************************************
 */

#ifndef ALP_PWM_H
#define ALP_PWM_H

#ifdef __cplusplus
extern "C" {
#endif
/************************************
 * INCLUDES
 ************************************/
#include "alp_driver_common.h"
#include "stdint.h"
#include "Driver_UTIMER.h"
#include "Driver_UTIMER.h"
#include "utimer.h"
/************************************
 * MACROS AND DEFINES
 ************************************/
// TODO: change them into ALP defines.

/**< UTIMER Events >*/
#define ALP_UTIMER_EVENT_CAPTURE_A                  ARM_UTIMER_EVENT_CAPTURE_A
#define ALP_UTIMER_EVENT_CAPTURE_B                  ARM_UTIMER_EVENT_CAPTURE_B
#define ALP_UTIMER_EVENT_COMPARE_A                  ARM_UTIMER_EVENT_COMPARE_A
#define ALP_UTIMER_EVENT_COMPARE_B                  ARM_UTIMER_EVENT_COMPARE_B
#define ALP_UTIMER_EVENT_COMPARE_A_BUF1             ARM_UTIMER_EVENT_COMPARE_A_BUF1
#define ALP_UTIMER_EVENT_COMPARE_A_BUF2             ARM_UTIMER_EVENT_COMPARE_A_BUF2
#define ALP_UTIMER_EVENT_COMPARE_B_BUF1             ARM_UTIMER_EVENT_COMPARE_B_BUF1
#define ALP_UTIMER_EVENT_COMPARE_B_BUF2             ARM_UTIMER_EVENT_COMPARE_B_BUF2
#define ALP_UTIMER_EVENT_UNDER_FLOW                 ARM_UTIMER_EVENT_UNDER_FLOW
#define ALP_UTIMER_EVENT_OVER_FLOW                  ARM_UTIMER_EVENT_OVER_FLOW

/**< UTIMER Channel declaration >*/
#define ALP_UTIMER_CHANNEL0                         ARM_UTIMER_CHANNEL0
#define ALP_UTIMER_CHANNEL1                         ARM_UTIMER_CHANNEL1
#define ALP_UTIMER_CHANNEL2                         ARM_UTIMER_CHANNEL2
#define ALP_UTIMER_CHANNEL3                         ARM_UTIMER_CHANNEL3
#define ALP_UTIMER_CHANNEL4                         ARM_UTIMER_CHANNEL4
#define ALP_UTIMER_CHANNEL5                         ARM_UTIMER_CHANNEL5
#define ALP_UTIMER_CHANNEL6                         ARM_UTIMER_CHANNEL6
#define ALP_UTIMER_CHANNEL7                         ARM_UTIMER_CHANNEL7
#define ALP_UTIMER_CHANNEL8                         ARM_UTIMER_CHANNEL8
#define ALP_UTIMER_CHANNEL9                         ARM_UTIMER_CHANNEL9
#define ALP_UTIMER_CHANNEL10                        ARM_UTIMER_CHANNEL10
#define ALP_UTIMER_CHANNEL11                        ARM_UTIMER_CHANNEL11
#define ALP_UTIMER_CHANNEL12                        ARM_UTIMER_CHANNEL12
#define ALP_UTIMER_CHANNEL13                        ARM_UTIMER_CHANNEL13
#define ALP_UTIMER_CHANNEL14                        ARM_UTIMER_CHANNEL14
#define ALP_UTIMER_CHANNEL15                        ARM_UTIMER_CHANNEL15

#define ALP_UTIMER_COUNTER_CLEAR                    ARM_UTIMER_COUNTER_CLEAR
#define ALP_UTIMER_COUNTER_NOT_CLEAR                ARM_UTIMER_COUNTER_NOT_CLEAR

/************************************
 * TYPEDEFS
 ************************************/
/**
 * enum ALP_UTIMER_MODE.
 * Driver UTIMER modes.
 */
typedef enum _ALP_UTIMER_MODE {
    ALP_UTIMER_MODE_BASIC,
    ALP_UTIMER_MODE_BUFFERING,
    ALP_UTIMER_MODE_TRIGGERING,
    ALP_UTIMER_MODE_CAPTURING,
    ALP_UTIMER_MODE_COMPARING,
    ALP_UTIMER_MODE_DEAD_TIME
} ALP_UTIMER_MODE;

/**
 * enum ALP_UTIMER_COUNTER_DIR.
 * Driver UTIMER counter direction.
 */
typedef enum _ALP_UTIMER_COUNTER_DIR {
    ALP_UTIMER_COUNTER_UP,
    ALP_UTIMER_COUNTER_DOWN,
    ALP_UTIMER_COUNTER_TRIANGLE
} ALP_UTIMER_COUNTER_DIR;

/**
 * enum ALP_UTIMER_COUNTER.
 * Driver UTIMER counters.
 */
typedef enum _ALP_UTIMER_COUNTER {
    ALP_UTIMER_CNTR,
    ALP_UTIMER_CNTR_PTR,
    ALP_UTIMER_CNTR_PTR_BUF1,
    ALP_UTIMER_CNTR_PTR_BUF2,
    ALP_UTIMER_DT_UP,
    ALP_UTIMER_DT_UP_BUF1,
    ALP_UTIMER_DT_DOWN,
    ALP_UTIMER_DT_DOWN_BUF1,
    ALP_UTIMER_COMPARE_A,
    ALP_UTIMER_COMPARE_B,
    ALP_UTIMER_COMPARE_A_BUF1,
    ALP_UTIMER_COMPARE_B_BUF1,
    ALP_UTIMER_COMPARE_A_BUF2,
    ALP_UTIMER_COMPARE_B_BUF2,
    ALP_UTIMER_CAPTURE_A,
    ALP_UTIMER_CAPTURE_B,
    ALP_UTIMER_CAPTURE_A_BUF1,
    ALP_UTIMER_CAPTURE_B_BUF1,
    ALP_UTIMER_CAPTURE_A_BUF2,
    ALP_UTIMER_CAPTURE_B_BUF2
} ALP_UTIMER_COUNTER;

/**
 * enum ALP_UTIMER_TRIGGER_SRC.
 * Driver UTIMER external event sources.
 */
typedef enum _ALP_UTIMER_TRIGGER_SRC {
    ALP_UTIMER_SRC_0,
    ALP_UTIMER_SRC_1,
    ALP_UTIMER_FAULT_TRIGGER,
    ALP_UTIMER_CNTR_PAUSE_TRIGGER
} ALP_UTIMER_TRIGGER_SRC;

/**
 * enum ALP_UTIMER_TRIGGER_TARGET.
 * Driver UTIMER trigger targets.
 */
typedef enum _ALP_UTIMER_TRIGGER_TARGET {
    ALP_UTIMER_TRIGGER_START,
    ALP_UTIMER_TRIGGER_STOP,
    ALP_UTIMER_TRIGGER_CLEAR,
    ALP_UTIMER_TRIGGER_UPCOUNT,
    ALP_UTIMER_TRIGGER_DOWNCOUNT,
    ALP_UTIMER_TRIGGER_CAPTURE_A,
    ALP_UTIMER_TRIGGER_CAPTURE_B,
    ALP_UTIMER_TRIGGER_DMA_CLEAR_A,
    ALP_UTIMER_TRIGGER_DMA_CLEAR_B
} ALP_UTIMER_TRIGGER_TARGET;

/**
 * enum ALP_UTIMER_TRIGGER.
 * Driver UTIMER trigger types.
 */
typedef enum _ALP_UTIMER_TRIGGER {
    ALP_UTIMER_SRC0_TRIG0_RISING,
    ALP_UTIMER_SRC0_TRIG0_FALLING,
    ALP_UTIMER_SRC0_TRIG1_RISING,
    ALP_UTIMER_SRC0_TRIG1_FALLING,
    ALP_UTIMER_SRC0_TRIG2_RISING,
    ALP_UTIMER_SRC0_TRIG2_FALLING,
    ALP_UTIMER_SRC0_TRIG3_RISING,
    ALP_UTIMER_SRC0_TRIG3_FALLING,
    ALP_UTIMER_SRC0_TRIG4_RISING,
    ALP_UTIMER_SRC0_TRIG4_FALLING,
    ALP_UTIMER_SRC0_TRIG5_RISING,
    ALP_UTIMER_SRC0_TRIG5_FALLING,
    ALP_UTIMER_SRC0_TRIG6_RISING,
    ALP_UTIMER_SRC0_TRIG6_FALLING,
    ALP_UTIMER_SRC0_TRIG7_RISING,
    ALP_UTIMER_SRC0_TRIG7_FALLING,
    ALP_UTIMER_SRC0_TRIG8_RISING,
    ALP_UTIMER_SRC0_TRIG8_FALLING,
    ALP_UTIMER_SRC0_TRIG9_RISING,
    ALP_UTIMER_SRC0_TRIG9_FALLING
} ALP_UTIMER_TRIGGER;

/** \brief ALP_UTIMER trigger configuration. */
typedef struct _ALP_UTIMER_TRIGGER_CONFIG {
    ALP_UTIMER_TRIGGER_TARGET     triggerTarget;
    ALP_UTIMER_TRIGGER_SRC        triggerSrc;
    ALP_UTIMER_TRIGGER            trigger;
} ALP_UTIMER_TRIGGER_CONFIG;


/************************************
 * EXPORTED VARIABLES
 ************************************/
// Add any extern variable declarations if necessary

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
static void ALP_UTIMER_Interrupt_Enable (ALP_UTIMER_RESOURCES *UTIMER_RES, uint8_t channel);


#ifdef __cplusplus
}
#endif

#endif /* ALP_DRIVER_TEMPLATE_H */