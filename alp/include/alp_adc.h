/**
 ********************************************************************************
 * @file    alp_adc.h
 * @author  Sukru Aydogdu
 * @date    30/01/2025
 * @brief   Header file for ALP ADC driver
 ********************************************************************************
 */

#ifndef ALP_ADC_H
#define ALP_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include "Driver_ADC.h"
#include "alp_driver_common.h"
#include "stdint.h"

/************************************
 * MACROS AND DEFINES
 ************************************/
// TODO: change them into ALP defines.
#define ALP_ADC_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR (1,0) /*API VERSION*/
#define ALP_ADC_DRV_VERISON ARM_DRIVER_VERSION_MAJOR_MINOR(1,0) /*DRIVER VERSION*/

// /**********ADC CONTROL CODE************/
// #define ARM_ADC_SHIFT_CTRL                           (0x01UL)          /* ARM ADC SHIFT CONTROL                  */
// #define ARM_ADC_SEQUENCER_CTRL                       (0x02UL)          /* ARM ADC SEQUENCER CONTROL              */
// #define ARM_ADC_SEQUENCER_MSK_CH_CTRL                (0x03UL)          /* ARM ADC SEQUENCER MASK CHANNEL CONTROL */
// #define ARM_ADC_CHANNEL_INIT_VAL                     (0x04UL)          /* ARM ADC CHANNEL INITIAL CONTROL        */
// #define ARM_ADC_COMPARATOR_A                         (0x05UL)          /* ARM ADC COMPARATOR A CONTROL           */
// #define ARM_ADC_COMPARATOR_B                         (0x06UL)          /* ARM ADC COMPARATOR B CONTROL           */
// #define ARM_ADC_THRESHOLD_COMPARISON                 (0X07UL)          /* ARM ADC THRESHOLD COMPARISON CONTROL   */
// #define ARM_ADC_CONVERSION_MODE_CTRL                 (0x08UL)          /* ARM ADC CONVERSION MODE CONTROL        */
// #define ARM_ADC_EXTERNAL_TRIGGER_ENABLE              (0x09UL)          /* ARM ADC EXTERNAL TRIGGER ENABLE        */
// #define ARM_ADC_EXTERNAL_TRIGGER_DISABLE             (0x0AUL)          /* ARM ADC EXTERNAL TRIGGER DISABLE       */
// #define ARM_ADC_HARDWARE_AVERAGING_CTRL              (0x0BUL)          /* ARM ADC SET HARDWARE AVERAGING         */
// #define ARM_ADC_SAMPLE_WIDTH_CTRL                    (0x0CUL)          /* ARM ADC SAMPLE WIDTH CONTROL           */
// #define ARM_ADC_DIFFERENTIAL_MODE_CTRL               (0x0DUL)          /* ARM ADC DIFFERENTIAL MODE CONTROL      */
// #define ARM_ADC_INPUT_CLOCK_DIV_CTRL                 (0x0FUL)          /* ARM ADC INPUT CLOCK DIVISOR CONTROL    */
// #define ARM_ADC_SET_PGA_GAIN_CTRL                    (0x10UL)          /* ARM ADC SET PGA GAIN CONTROL           */
// #define ARM_ADC_24_BIAS_CTRL                         (0x11UL)          /* ARM ADC 24 BIAS CONTROL                */
// #define ARM_ADC_24_OUTPUT_RATE_CTRL                  (0x12UL)          /* ARM ADC 24 OUTPUT RATE CONTROL         */

// /*********THRESHOLD COMPARSION**********/
// #define ARM_ADC_ABOVE_A_AND_ABOVE_B                  (0x00UL)          /* ARM ADC THRESHOLD ABOVE A AND ABOVE B         */
// #define ARM_ADC_BELOW_A_AND_BELOW_B                  (0x01UL)          /* ARM ADC THRESHOLD BELOW A AND BELOW B         */
// #define ARM_ADC_BETWEEN_A_B_AND_OUTSIDE_A_B          (0x02UL)          /* ARM ADC THRESHOLD BETWEEN A_B AND OUTSIDE A_B */

// /**********ADC EVENT********************/
// #define ARM_ADC_EVENT_CONVERSION_COMPLETE            (1 << 0)          /* ARM ADC EVENT CONVERSION COMPLETE        */
// #define ARM_ADC_COMPARATOR_THRESHOLD_ABOVE_A         (1 << 1)          /* ARM ADC COMPARATOR THRESHOLD ABOVE A     */
// #define ARM_ADC_COMPARATOR_THRESHOLD_ABOVE_B         (1 << 2)          /* ARM ADC COMPARATOR THRESHOLD ABOVE B     */
// #define ARM_ADC_COMPARATOR_THRESHOLD_BELOW_A         (1 << 3)          /* ARM ADC COMPARATOR THRESHOLD BELOW A     */
// #define ARM_ADC_COMPARATOR_THRESHOLD_BELOW_B         (1 << 4)          /* ARM ADC COMPARATOR THRESHOLD BELOW B     */
// #define ARM_ADC_COMPARATOR_THRESHOLD_BETWEEN_A_B     (1 << 5)          /* ARM ADC COMPARATOR THRESHOLD BETWEEN A_B */
// #define ARM_ADC_COMPARATOR_THRESHOLD_OUTSIDE_A_B     (1 << 6)          /* ARM ADC COMPARATOR THRESHOLD OUTSIDE A_B */

// /**********ADC CONVERSION OPERATION**********/
// #define ARM_ADC_CONTINOUS_CH_CONV                    (0x00)            /* ARM ADC CHANNEL CONTINUOUS CONVERSION    */
// #define ARM_ADC_SINGLE_SHOT_CH_CONV                  (0x01)            /* ARM ADC CHANNEL SINGLE CONVERSION        */

// /**********ADC SCAN OPERATION**********/
// #define ARM_ADC_MULTIPLE_CH_SCAN                     (0x00)            /* ARM ADC MULTIPLE CHANNEL SCAN MODE */
// #define ARM_ADC_SINGLE_CH_SCAN                       (0x01)            /* ARM ADC SINGLE CHANNEL SCAN MODE   */

// /**********ADC CHANNELS******/
// #define ARM_ADC_CHANNEL_0                             (0x00)           /* ARM ADC CHANNEL 0 */
// #define ARM_ADC_CHANNEL_1                             (0x01)           /* ARM ADC CHANNEL 1 */
// #define ARM_ADC_CHANNEL_2                             (0x02)           /* ARM ADC CHANNEL 2 */
// #define ARM_ADC_CHANNEL_3                             (0x03)           /* ARM ADC CHANNEL 3 */
// #define ARM_ADC_CHANNEL_4                             (0x04)           /* ARM ADC CHANNEL 4 */
// #define ARM_ADC_CHANNEL_5                             (0x05)           /* ARM ADC CHANNEL 5 */
// #define ARM_ADC_CHANNEL_6                             (0x06)           /* ARM ADC CHANNEL 6 */
// #define ARM_ADC_CHANNEL_7                             (0x07)           /* ARM ADC CHANNEL 7 */
// #define ARM_ADC_CHANNEL_8                             (0x08)           /* ARM ADC CHANNEL 8 */

// /****ADC MASK CHANNEL****/
// #define ARM_ADC_MASK_CHANNEL_0                        (1 << ARM_ADC_CHANNEL_0)           /* ARM ADC MASK CHANNEL 0 */
// #define ARM_ADC_MASK_CHANNEL_1                        (1 << ARM_ADC_CHANNEL_1)           /* ARM ADC MASK CHANNEL 1 */
// #define ARM_ADC_MASK_CHANNEL_2                        (1 << ARM_ADC_CHANNEL_2)           /* ARM ADC MASK CHANNEL 2 */
// #define ARM_ADC_MASK_CHANNEL_3                        (1 << ARM_ADC_CHANNEL_3)           /* ARM ADC MASK CHANNEL 3 */
// #define ARM_ADC_MASK_CHANNEL_4                        (1 << ARM_ADC_CHANNEL_4)           /* ARM ADC MASK CHANNEL 4 */
// #define ARM_ADC_MASK_CHANNEL_5                        (1 << ARM_ADC_CHANNEL_5)           /* ARM ADC MASK CHANNEL 5 */
// #define ARM_ADC_MASK_CHANNEL_6                        (1 << ARM_ADC_CHANNEL_6)           /* ARM ADC MASK CHANNEL 6 */
// #define ARM_ADC_MASK_CHANNEL_7                        (1 << ARM_ADC_CHANNEL_7)           /* ARM ADC MASK CHANNEL 7 */
// #define ARM_ADC_MASK_CHANNEL_8                        (1 << ARM_ADC_CHANNEL_8)           /* ARM ADC MASK CHANNEL 8 */

// /* External trigger macros */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_0               (1UL << 0)           /* ARM ADC EXTERNAL TRIGGER SOURCE 0 */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_1               (1UL << 1)           /* ARM ADC EXTERNAL TRIGGER SOURCE 1 */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_2               (1UL << 2)           /* ARM ADC EXTERNAL TRIGGER SOURCE 2 */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_3               (1UL << 3)           /* ARM ADC EXTERNAL TRIGGER SOURCE 3 */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_4               (1UL << 4)           /* ARM ADC EXTERNAL TRIGGER SOURCE 4 */
// #define ARM_ADC_EXTERNAL_TRIGGER_SRC_5               (1UL << 5)           /* ARM ADC EXTERNAL TRIGGER SOURCE 5 */


/************************************
 * TYPEDEFS
 ************************************/
typedef void (*ARM_ADC_SignalEvent_t) (uint32_t event, uint8_t channel, uint32_t value);    /*Pointer to \ref ADC_SignalEvent : Signal ADC Event*/

/* Enumeration for SPI Instances */
typedef enum
{
    ALP_ADC_0,
    ALP_ADC_1,
    ALP_ADC_2,
    ALP_ADC_3,
} ALP_ADC_Instance;

typedef struct _ALP_ADC_CAPABILITIES{
    uint32_t Resolution         :1;     /* Resolution 12 or 20 bits*/
    uint32_t Reserved           :31;    /* Reserved                */
}ALP_ADC_CAPABILITIES;

/* Driver Version */
static const ARM_DRIVER_VERSION DriverVersion ={
    ALP_ADC_API_VERSION,
    ALP_ADC_DRV_VERISON
};

/* Driver Capabilities */
static const ALP_ADC_CAPABILITIES DriverCapabilities = {
    1,    /* Resolution 12 or 20 bits*/
    0     /* Reserved                */
};
/************************************
 * EXPORTED VARIABLES
 ************************************/
// Add any extern variable declarations if necessary

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
ARM_DRIVER_VERSION ALP_ADC_GetVersion(ALP_ADC_Instance adc_instance);
ARM_ADC_CAPABILITIES ALP_ADC_GetCapabilities(ALP_ADC_Instance adc_instance);
int32_t ALP_ADC_Initialize(ALP_ADC_Instance adc_instance,ARM_ADC_SignalEvent_t cb_event);
int32_t ALP_ADC_Uninitialize(ALP_ADC_Instance adc_instance);
int32_t ALP_ADC_Start(ALP_ADC_Instance adc_instance);
int32_t ALP_ADC_Stop(ALP_ADC_Instance adc_instance);
int32_t ALP_ADC_PowerControl(ALP_ADC_Instance adc_instance, ARM_POWER_STATE state);
int32_t ALP_ADC_Control(ALP_ADC_Instance adc_instance, uint32_t Control, uint32_t arg);

#ifdef __cplusplus
}
#endif

#endif /* ALP_DRIVER_TEMPLATE_H */
