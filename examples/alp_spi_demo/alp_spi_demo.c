/**
 ********************************************************************************
 * @file    alp_spi_demo.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP SDK SPI Demo - Full demonstration using ALP SDK functions
 * @note    Demonstrates SPI, GPIO, and USART together on Alif E7 DevKit
 ********************************************************************************
 */

#include "alp_spi_vft.h"
#include "alp_gpio_vft.h"
#include "alp_usart_vft.h"
#include "alp_board.h"
#include <stdio.h>
#include <string.h>

// Simple delay
static void delay_ms(uint32_t ms)
{
    for (volatile uint32_t i = 0; i < ms * 10000; i++);
}

int main(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║         ALP SDK - Complete Demo (SPI + GPIO + USART)      ║\n");
    printf("║         Platform: Alif E7 DevKit Gen2                      ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n\n");
    
    // Get board configuration
    const alp_board_config_t *board = alp_board_get_config();
    printf("Board: %s\n", board->name);
    printf("LED1: GPIO%d.%d | LED2: GPIO%d.%d\n", 
           board->gpio.led1.port, board->gpio.led1.pin,
           board->gpio.led2.port, board->gpio.led2.pin);
    printf("USART: Instance %d @ %d baud\n\n", 
           board->usart.instance, board->usart.baudrate);
    
    // 1. Initialize USART for logging
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("1. Initializing USART for logging...\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    
    alp_usart_handle_t *usart = alp_usart_create_alif(board->usart.instance);
    if (!usart) {
        printf("ERROR: Failed to create USART handle\n");
        return -1;
    }
    
    alp_usart_config_t usart_config = {
        .instance = board->usart.instance,
        .baudrate = board->usart.baudrate,
        .data_bits = ALP_USART_DATA_BITS_8,
        .parity = ALP_USART_PARITY_NONE,
        .stop_bits = ALP_USART_STOP_BITS_1,
        .flow_control = ALP_USART_FLOW_CONTROL_NONE
    };
    
    if (alp_usart_init(usart, &usart_config, NULL) != ALP_USART_OK) {
        printf("ERROR: USART init failed\n");
        return -1;
    }
    
    alp_usart_printf(usart, "\r\n✅ USART initialized successfully\r\n");
    printf("✅ USART ready\n\n");
    
    // 2. Initialize GPIO LEDs
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("2. Initializing GPIO LEDs...\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    
    alp_gpio_handle_t *led1 = alp_gpio_create_alif(board->gpio.led1.port, board->gpio.led1.pin);
    alp_gpio_handle_t *led2 = alp_gpio_create_alif(board->gpio.led2.port, board->gpio.led2.pin);
    
    if (!led1 || !led2) {
        alp_usart_printf(usart, "ERROR: Failed to create GPIO handles\r\n");
        return -1;
    }
    
    alp_gpio_config_t led_config = {
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    led_config.port = board->gpio.led1.port;
    led_config.pin = board->gpio.led1.pin;
    alp_gpio_init(led1, &led_config, NULL);
    
    led_config.port = board->gpio.led2.port;
    led_config.pin = board->gpio.led2.pin;
    alp_gpio_init(led2, &led_config, NULL);
    
    alp_usart_printf(usart, "✅ GPIO LEDs initialized\r\n");
    printf("✅ GPIO ready\n\n");
    
    // 3. Initialize SPI
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("3. Initializing SPI...\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    
    alp_spi_handle_t *spi = alp_spi_create_alif(0);
    if (!spi) {
        alp_usart_printf(usart, "ERROR: Failed to create SPI handle\r\n");
        return -1;
    }
    
    alp_spi_config_t spi_config = {
        .instance = 0,
        .mode = ALP_SPI_MODE_MASTER,
        .baudrate = 1000000,
        .data_bits = 8,
        .clock_mode = ALP_SPI_CPOL0_CPHA0,
        .bit_order = ALP_SPI_MSB_FIRST,
        .ss_mode = ALP_SPI_SS_MASTER_SW
    };
    
    if (alp_spi_init(spi, &spi_config, NULL) != ALP_STATUS_OK) {
        alp_usart_printf(usart, "ERROR: SPI init failed\r\n");
        return -1;
    }
    
    alp_usart_printf(usart, "✅ SPI initialized @ %d Hz\r\n", spi_config.baudrate);
    printf("✅ SPI ready\n\n");
    
    // 4. Main demo loop
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("4. Starting main demo loop...\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    
    alp_usart_printf(usart, "\r\n");
    alp_usart_printf(usart, "╔════════════════════════════════════════╗\r\n");
    alp_usart_printf(usart, "║     ALP SDK Demo Running!              ║\r\n");
    alp_usart_printf(usart, "╚════════════════════════════════════════╝\r\n");
    alp_usart_printf(usart, "\r\n");
    
    uint32_t iteration = 0;
    uint8_t tx_data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0xAA, 0xBB, 0xCC};
    uint8_t rx_data[8] = {0};
    
    while (1) {
        iteration++;
        
        // TODO: GPIO support
        // alp_gpio_toggle(led1);
        // alp_usart_printf(usart, "[%lu] LED1 toggled | ", iteration);
        
        // Send SPI data
        alp_spi_send(spi, tx_data, 8);
        alp_usart_printf(usart, "[%lu] SPI: TX 8 bytes | ", iteration);
        
        delay_ms(100);
        
        // TODO: GPIO support
        // alp_gpio_toggle(led2);
        // alp_usart_printf(usart, "LED2 toggled | ");
        
        // Receive SPI data (in real scenario with connected device)
        alp_spi_receive(spi, rx_data, 8);
        alp_usart_printf(usart, "RX 8 bytes\r\n");
        
        delay_ms(400);
        
        // Send status every 5 iterations
        if (iteration % 5 == 0) {
            alp_usart_printf(usart, "\r\n");
            alp_usart_printf(usart, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n");
            alp_usart_printf(usart, "  Status: %lu iterations completed\r\n", iteration);
            alp_usart_printf(usart, "  Using: ALP SDK on Alif E7\r\n");
            alp_usart_printf(usart, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n");
            alp_usart_printf(usart, "\r\n");
        }
    }
    
    // Cleanup (never reached)
    alp_spi_deinit(spi);
    // alp_gpio_deinit(led1);  // TODO: GPIO support
    // alp_gpio_deinit(led2);
    alp_usart_deinit(usart);
    
    return 0;
}
