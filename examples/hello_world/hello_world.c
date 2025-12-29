/**
 ********************************************************************************
 * @file    hello_world.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Hello World Example - USART printf using ALP USART API
 * @note    Demonstrates platform-independent serial communication
 ********************************************************************************
 */

#include "alp_usart_vft.h"
#include "alp_board.h"
#include <stdio.h>
#include <stdint.h>

// Platform selection - change this to switch platforms
#ifdef BUILD_ALIF
    #define PLATFORM_NAME "Alif E7"
    #define CREATE_USART(instance) alp_usart_create_alif(instance)
#else
    #define PLATFORM_NAME "Mock"
    #define CREATE_USART(instance) alp_usart_create_mock(instance)
#endif

// Simple delay function (replace with proper timer in production)
static void delay_ms(uint32_t ms)
{
    for (volatile uint32_t i = 0; i < ms * 10000; i++);
}

int main(void)
{
    printf("\n");
    printf("==============================================\n");
    printf("  ALP SDK - Hello World Example\n");
    printf("  Platform: %s\n", PLATFORM_NAME);
    printf("==============================================\n\n");
    
    // Get board configuration
    const alp_board_config_t *board = alp_board_get_config();
    printf("Board: %s\n", board->name);
    printf("USART: Instance %d, Baudrate %d\n\n", board->usart.instance, board->usart.baudrate);
    
    // Create USART handle
    alp_usart_handle_t *usart = CREATE_USART(board->usart.instance);
    
    if (!usart) {
        printf("ERROR: Failed to create USART handle\n");
        return -1;
    }
    
    // Configure USART
    alp_usart_config_t usart_config = {
        .instance = board->usart.instance,
        .baudrate = board->usart.baudrate,
        .data_bits = ALP_USART_DATA_BITS_8,
        .parity = ALP_USART_PARITY_NONE,
        .stop_bits = ALP_USART_STOP_BITS_1,
        .flow_control = ALP_USART_FLOW_CONTROL_NONE
    };
    
    // Initialize USART
    if (alp_usart_init(usart, &usart_config, NULL) != ALP_USART_OK) {
        printf("ERROR: Failed to initialize USART\n");
        return -1;
    }
    
    printf("✅ USART initialized successfully\n\n");
    printf("Starting hello world messages...\n");
    printf("(Press Ctrl+C to stop)\n\n");
    
    // Hello world loop
    uint32_t count = 0;
    while (1) {
        count++;
        
        // Send "Hello, World!" message
        alp_usart_printf(usart, "Hello, World! [Message #%d]\r\n", count);
        
        // Also print to console for Mock platform
        printf("[%d] Sent: Hello, World!\n", count);
        
        delay_ms(1000);
        
        // Send platform info
        alp_usart_printf(usart, "Platform: %s | Board: %s\r\n", 
                         PLATFORM_NAME, board->name);
        
        printf("[%d] Sent: Platform info\n", count);
        
        delay_ms(2000);
    }
    
    // Cleanup (never reached in this example)
    alp_usart_deinit(usart);
    
    return 0;
}
