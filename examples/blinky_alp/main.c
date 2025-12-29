/**
 ********************************************************************************
 * @file    main.c
 * @brief   Blinky Example for Alif Ensemble E8 DevKit using ALP SDK
 * @note    Toggles RGB LED using ALP GPIO abstraction layer
 * 
 * This example demonstrates:
 * - ALP SDK GPIO abstraction API
 * - Platform-independent LED control
 * - LED toggling using SysTick timer
 ********************************************************************************
 */

#include "RTE_Components.h"
#include CMSIS_device_header

#include "board_config.h"
#include "alp_gpio_vft.h"

// LED GPIO handles
static alp_gpio_handle_t *led_blue = NULL;
static alp_gpio_handle_t *led_red = NULL;

// LED pin definitions based on board configuration
#if defined(BOARD_RGB_LED_INSTANCE) && (BOARD_RGB_LED_INSTANCE == 0)
    #define LED_BLUE_PORT BOARD_LEDRGB0_B_GPIO_PORT
    #define LED_BLUE_PIN  BOARD_LEDRGB0_B_GPIO_PIN
    #define LED_RED_PORT  BOARD_LEDRGB0_R_GPIO_PORT
    #define LED_RED_PIN   BOARD_LEDRGB0_R_GPIO_PIN
#else
    #define LED_BLUE_PORT BOARD_LEDRGB1_B_GPIO_PORT
    #define LED_BLUE_PIN  BOARD_LEDRGB1_B_GPIO_PIN
    #define LED_RED_PORT  BOARD_LEDRGB1_R_GPIO_PORT
    #define LED_RED_PIN   BOARD_LEDRGB1_R_GPIO_PIN
#endif

/**
 * @brief  Main program entry point
 */
int main (void)
{
    // Initialize board pinmux configuration
    int32_t ret = board_pins_config();
    if (ret != 0) {
        // Configuration failed - halt
        while(1);
    }

    // Create Blue LED GPIO handle using ALP SDK
    led_blue = alp_gpio_create_alif(LED_BLUE_PORT, LED_BLUE_PIN);
    if (!led_blue) {
        while(1);  // Failed to create GPIO handle
    }

    // Configure Blue LED
    alp_gpio_config_t blue_config = {
        .port = LED_BLUE_PORT,
        .pin = LED_BLUE_PIN,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    if (alp_gpio_init(led_blue, &blue_config, NULL) != ALP_GPIO_OK) {
        while(1);  // Failed to initialize
    }

    // Set initial state to LOW
    alp_gpio_write(led_blue, ALP_GPIO_VALUE_LOW);

    // Create Red LED GPIO handle using ALP SDK
    led_red = alp_gpio_create_alif(LED_RED_PORT, LED_RED_PIN);
    if (!led_red) {
        while(1);
    }

    // Configure Red LED
    alp_gpio_config_t red_config = {
        .port = LED_RED_PORT,
        .pin = LED_RED_PIN,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    if (alp_gpio_init(led_red, &red_config, NULL) != ALP_GPIO_OK) {
        while(1);
    }

    // Set initial state to LOW
    alp_gpio_write(led_red, ALP_GPIO_VALUE_LOW);

    // Configure SysTick timer for LED toggling
    #ifdef CORE_M55_HE
    SysTick_Config(SystemCoreClock/10);  // 10 Hz toggle for HE core
    #else
    SysTick_Config(SystemCoreClock/25);  // 25 Hz toggle for other cores
    #endif

    // Main loop - wait for interrupts
    while (1) {
        __WFI();  // Wait For Interrupt
    }
}

/**
 * @brief  SysTick interrupt handler - toggles LED using ALP SDK
 */
void SysTick_Handler (void)
{
    #ifdef CORE_M55_HE
    if (led_blue) {
        alp_gpio_toggle(led_blue);  // Use ALP SDK toggle function
    }
    #else
    if (led_red) {
        alp_gpio_toggle(led_red);  // Use ALP SDK toggle function
    }
    #endif
}

