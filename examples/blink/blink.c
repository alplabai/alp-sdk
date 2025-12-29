/**
 ********************************************************************************
 * @file    blink.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Blink Example - LED Toggle using ALP GPIO API
 * @note    Demonstrates platform-independent LED control
 ********************************************************************************
 */

#include "alp_gpio_vft.h"
#include "alp_board.h"
#include <stdio.h>
#include <stdint.h>

// Platform selection - change this to switch platforms
#ifdef BUILD_ALIF
    #define PLATFORM_NAME "Alif E7"
    #define CREATE_GPIO(port, pin) alp_gpio_create_alif(port, pin)
#else
    #define PLATFORM_NAME "Mock"
    #define CREATE_GPIO(port, pin) alp_gpio_create_mock(port, pin)
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
    printf("  ALP SDK - Blink Example\n");
    printf("  Platform: %s\n", PLATFORM_NAME);
    printf("==============================================\n\n");
    
    // Get board configuration
    const alp_board_config_t *board = alp_board_get_config();
    printf("Board: %s\n", board->name);
    printf("LED1: GPIO Port %d, Pin %d\n", board->gpio.led1.port, board->gpio.led1.pin);
    printf("LED2: GPIO Port %d, Pin %d\n\n", board->gpio.led2.port, board->gpio.led2.pin);
    
    // Create GPIO handles for LEDs
    alp_gpio_handle_t *led1 = CREATE_GPIO(board->gpio.led1.port, board->gpio.led1.pin);
    alp_gpio_handle_t *led2 = CREATE_GPIO(board->gpio.led2.port, board->gpio.led2.pin);
    
    if (!led1 || !led2) {
        printf("ERROR: Failed to create GPIO handles\n");
        return -1;
    }
    
    // Configure LED1
    alp_gpio_config_t led1_config = {
        .port = board->gpio.led1.port,
        .pin = board->gpio.led1.pin,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    // Configure LED2
    alp_gpio_config_t led2_config = {
        .port = board->gpio.led2.port,
        .pin = board->gpio.led2.pin,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    // Initialize GPIOs
    if (alp_gpio_init(led1, &led1_config, NULL) != ALP_GPIO_OK) {
        printf("ERROR: Failed to initialize LED1\n");
        return -1;
    }
    
    if (alp_gpio_init(led2, &led2_config, NULL) != ALP_GPIO_OK) {
        printf("ERROR: Failed to initialize LED2\n");
        return -1;
    }
    
    printf("✅ GPIOs initialized successfully\n\n");
    printf("Starting blink sequence...\n");
    printf("(Press Ctrl+C to stop)\n\n");
    
    // Blink loop
    uint32_t count = 0;
    while (1) {
        count++;
        
        // Toggle LED1
        alp_gpio_toggle(led1);
        printf("[%d] LED1 toggled\n", count);
        
        delay_ms(500);
        
        // Toggle LED2
        alp_gpio_toggle(led2);
        printf("[%d] LED2 toggled\n", count);
        
        delay_ms(500);
    }
    
    // Cleanup (never reached in this example)
    alp_gpio_deinit(led1);
    alp_gpio_deinit(led2);
    
    return 0;
}
