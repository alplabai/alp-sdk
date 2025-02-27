/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include <time.h>
#include <stdint.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include <stdio.h>
#include "alp_adc.h"
#include "board.h"
#include "uart_tracelib.h"
#include "fault_handler.h"

#include "system_utils.h"

/* include for ADC Driver */
#include "Driver_ADC.h"

/* PINMUX include */
#include "pinconf.h"

#include "se_services_port.h"

/* single shot conversion scan use ARM_ADC_SINGLE_SHOT_CH_CONV*/
#define ADC_CONVERSION    ARM_ADC_SINGLE_SHOT_CH_CONV

#define POTENTIOMETER            ARM_ADC_CHANNEL_1
#define NUM_CHANNELS             (8)

/* Demo purpose adc_sample*/
uint32_t adc_sample[NUM_CHANNELS];

volatile uint32_t num_samples = 0;

/*
 * @func   : void adc_conversion_callback(uint32_t event, uint8_t channel, uint32_t sample_output)
 * @brief  : adc conversion isr callback
 * @return : NONE
*/
static void adc_conversion_callback(uint32_t event, uint8_t channel, uint32_t sample_output)
{
    BOARD_LED2_Control(BOARD_LED_STATE_TOGGLE);
    if (event & ARM_ADC_EVENT_CONVERSION_COMPLETE)
    {
        num_samples += 1;

        /* Store the value for the respected channels */
        adc_sample[channel] = sample_output;
    }
}

/**
 *    @func   : void adc_potentiometer_demo()
 *    @brief  : ADC Potentiometer demo
 *             - test to verify the potentiometer analog input
 *             - Internal input of potentiometer in analog signal corresponding
 *               output is digital value.
 *             - converted value is the allocated user memory address.
 *    @return : NONE
*/
void adc_potentiometer_demo()
{
    int32_t  ret                = 0;
    uint32_t error_code         = SERVICES_REQ_SUCCESS;
    uint32_t service_error_code;
    ARM_DRIVER_VERSION version;

    /* Initialize the SE services */
    se_services_port_init();

    /* enable the 160 MHz clock */
    error_code = SERVICES_clocks_enable_clock(se_services_s_handle,
                           /*clock_enable_t*/ CLKEN_CLK_160M,
                           /*bool enable   */ true,
                                              &service_error_code);
    if(error_code)
    {
        printf("SE Error: 160 MHz clk enable = %d\n", error_code);
        return;
    }

    printf("\t\t\n >>> ADC demo starting up!!! <<< \r\n");

    version = ALP_ADC_GetVersion(ALP_ADC_1);
    printf("\r\n ADC version api:%X driver:%X...\r\n",version.api, version.drv);

    /* Initialize ADC driver */
    ret = ALP_ADC_Initialize(ALP_ADC_1, adc_conversion_callback);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC init failed\n");
        return;
    }

    /* Power control ADC */
    ret = ALP_ADC_PowerControl(ALP_ADC_1, ARM_POWER_FULL);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC Power up failed\n");
        goto error_uninitialize;
    }

    /* set conversion mode */
    ret = ALP_ADC_Control(ALP_ADC_1, ARM_ADC_CONVERSION_MODE_CTRL, ADC_CONVERSION);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC select conversion mode failed\n");
        goto error_poweroff;
    }

    /* set initial channel */
    ret = ALP_ADC_Control(ALP_ADC_1, ARM_ADC_CHANNEL_INIT_VAL, POTENTIOMETER);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC channel init failed\n");
        goto error_poweroff;
    }

    printf(">>> Allocated memory buffer Address is 0x%X <<<\n",(uint32_t)(adc_sample + POTENTIOMETER));
    /* Start ADC */
    ret = ALP_ADC_Start(ALP_ADC_1);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC Start failed\n");
        goto error_poweroff;
    }

    /* wait for timeout */
    while(!(num_samples == 1));
    
    while(1)
    {
        ret = ALP_ADC_Start(ALP_ADC_1);
        if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC Start failed\n");
        goto error_poweroff;
        }

        printf("Value in memory = %d\n\n", *(int*)0x20001260);
        for(int i=0; i<NUM_CHANNELS; i++)
        {
            printf("ADC[%d] = %d\n", i, adc_sample[i]);
        }
        for(int i=0; i<10; i++)
        {
            sys_busy_loop_us(100000);
        }
    }
    printf("\n Potentiometer conversion completed \n");

    /* Stop ADC */
    ret = ALP_ADC_Stop(ALP_ADC_1);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: ADC Stop failed\n");
        goto error_poweroff;
    }

    printf("\n ---END--- \r\n wait forever >>> \n");
    while(1);

error_poweroff:

    /* Power off ADC peripheral */
    ret = ALP_ADC_PowerControl(ALP_ADC_1, ARM_POWER_OFF);
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: ADC Power OFF failed.\r\n");
    }

error_uninitialize:

    /* Un-initialize ADC driver */
    ret = ALP_ADC_Uninitialize(ALP_ADC_1);
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: ADC Uninitialize failed.\r\n");
    }
    /* disable the 160MHz clock */
    error_code = SERVICES_clocks_enable_clock(se_services_s_handle,
                           /*clock_enable_t*/ CLKEN_CLK_160M,
                           /*bool enable   */ false,
                                              &service_error_code);
    if(error_code)
    {
        printf("SE Error: 160 MHz clk disable = %d\n", error_code);
        return;
    }

    printf("\r\n ADC demo exiting...\r\n");
}

static void uart_callback(uint32_t event)
{
}

int main (void)
{
    // Init pinmux using boardlib
    BOARD_Pinmux_Init();
    
    // Set pinmux for ADC
	pinconf_set(PORT_0, PIN_7, PINMUX_ALTERNATE_FUNCTION_7, PADCTRL_READ_ENABLE);

    sys_busy_loop_init();

    // Use common_app_utils for printing
    tracelib_init(NULL, uart_callback);

    fault_dump_enable(true);

    BOARD_LED2_Control(BOARD_LED_STATE_HIGH);

    adc_potentiometer_demo();

    printf("\r\nADC demo ended \r\n");

    while (1) __WFE();
}
