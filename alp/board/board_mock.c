/**
 ********************************************************************************
 * @file    board_mock.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Mock Board Configuration
 * @note    Simulated board for testing without hardware
 ********************************************************************************
 */

#include "alp_board.h"

/************************************
 * BOARD CONFIGURATION
 ************************************/

static const alp_board_config_t mock_board_config = {
    .name = "Mock Board (Simulated)",
    .type = ALP_BOARD_MOCK,
    
    .gpio = {
        .led1 = { .port = 0, .pin = 0 },
        .led2 = { .port = 0, .pin = 1 },
        .button1 = { .port = 0, .pin = 2 },
        .button2 = { .port = 0, .pin = 3 }
    },
    
    .usart = {
        .instance = 0,
        .baudrate = 115200
    }
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

const alp_board_config_t* alp_board_get_config(void)
{
    return &mock_board_config;
}

bool alp_board_init(void)
{
    // Mock board initialization always succeeds
    return true;
}
