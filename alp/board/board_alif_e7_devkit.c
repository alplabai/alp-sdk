/**
 ********************************************************************************
 * @file    board_alif_e7_devkit.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Alif E7 DevKit Gen2 Board Configuration
 * @note    Based on delphisonic-e1m board definitions (variant 4)
 ********************************************************************************
 */

#include "alp_board.h"

/************************************
 * BOARD CONFIGURATION
 * From: delphisonic-e1m/device/alif-ensemble/Board/devkit_gen2/
 ************************************/

static const alp_board_config_t alif_e7_devkit_config = {
    .name = "Alif E7 DevKit Gen2",
    .type = ALP_BOARD_ALIF_E7_DEVKIT,
    
    .gpio = {
        // LED1: GPIO Port 12, Pin 3
        .led1 = { .port = 12, .pin = 3 },
        
        // LED2: GPIO Port 1, Pin 14
        .led2 = { .port = 1, .pin = 14 },
        
        // BUTTON1: GPIO Port 12, Pin 2
        .button1 = { .port = 12, .pin = 2 },
        
        // BUTTON2: GPIO Port 1, Pin 12
        .button2 = { .port = 1, .pin = 12 }
    },
    
    .usart = {
        // UART2 for E7 DevKit
        .instance = 2,
        .baudrate = 115200
    }
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

const alp_board_config_t* alp_board_get_config(void)
{
    return &alif_e7_devkit_config;
}

bool alp_board_init(void)
{
    // Board-specific initialization if needed
    return true;
}
