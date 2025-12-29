/**
 ********************************************************************************
 * @file    main.c
 * @brief   Hello World Example for Alif Ensemble E8 DevKit using ALP SDK
 * @note    Demonstrates ALP SDK GPIO with alternating LED pattern
 * 
 * This example demonstrates:
 * - ALP SDK GPIO abstraction API
 * - Platform-independent peripheral control
 * - Multi-LED coordination
 ********************************************************************************
 */

#include "RTE_Components.h"
#include CMSIS_device_header

#include "board_config.h"
#include "alp_gpio_vft.h"

// LED GPIO handles
static alp_gpio_handle_t *led_red = NULL;
static alp_gpio_handle_t *led_green = NULL;

// LED pin definitions
#if defined(BOARD_RGB_LED_INSTANCE) && (BOARD_RGB_LED_INSTANCE == 0)
    #define LED_RED_PORT   BOARD_LEDRGB0_R_GPIO_PORT
    #define LED_RED_PIN    BOARD_LEDRGB0_R_GPIO_PIN
    #define LED_GREEN_PORT BOARD_LEDRGB0_G_GPIO_PORT
    #define LED_GREEN_PIN  BOARD_LEDRGB0_G_GPIO_PIN
#else
    #define LED_RED_PORT   BOARD_LEDRGB1_R_GPIO_PORT
    #define LED_RED_PIN    BOARD_LEDRGB1_R_GPIO_PIN
    #define LED_GREEN_PORT BOARD_LEDRGB1_G_GPIO_PORT
    #define LED_GREEN_PIN  BOARD_LEDRGB1_G_GPIO_PIN
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

    // Create Red LED GPIO handle using ALP SDK
    led_red = alp_gpio_create_alif(LED_RED_PORT, LED_RED_PIN);
    if (!led_red) {
        while(1);  // Failed to create GPIO handle
    }

    // Configure Red LED as output
    alp_gpio_config_t red_config = {
        .port = LED_RED_PORT,
        .pin = LED_RED_PIN,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    if (alp_gpio_init(led_red, &red_config, NULL) != ALP_GPIO_OK) {
        while(1);  // Failed to initialize
    }

    // Create Green LED GPIO handle using ALP SDK
    led_green = alp_gpio_create_alif(LED_GREEN_PORT, LED_GREEN_PIN);
    if (!led_green) {
        while(1);
    }

    // Configure Green LED as output
    alp_gpio_config_t green_config = {
        .port = LED_GREEN_PORT,
        .pin = LED_GREEN_PIN,
        .direction = ALP_GPIO_DIRECTION_OUTPUT,
        .pull = ALP_GPIO_PULL_NONE,
        .irq_mode = ALP_GPIO_IRQ_NONE
    };
    
    if (alp_gpio_init(led_green, &green_config, NULL) != ALP_GPIO_OK) {
        while(1);
    }

    // Set initial states - Red ON, Green OFF
    alp_gpio_write(led_red, ALP_GPIO_VALUE_HIGH);
    alp_gpio_write(led_green, ALP_GPIO_VALUE_LOW);

    // Configure SysTick timer for LED alternating pattern
    SysTick_Config(SystemCoreClock/5);  // 5 Hz toggle (slower than blinky)

    // Main loop - wait for interrupts
    while (1) {
        __WFI();  // Wait For Interrupt
    }
}

/**
 * @brief  SysTick interrupt handler - alternates LEDs using ALP SDK
 */
void SysTick_Handler (void)
{
    // Alternate between Red and Green LEDs
    if (led_red && led_green) {
        alp_gpio_toggle(led_red);    // Toggle Red LED
        alp_gpio_toggle(led_green);  // Toggle Green LED (opposite of Red)
    }
}

// Stubs to suppress missing stdio definitions for nosys
#define TRAP_RET_ZERO  {__BKPT(0); return 0;}
int _close(int val) TRAP_RET_ZERO
int _lseek(int val0, int val1, int val2) TRAP_RET_ZERO
int _read(int val0, char * val1, int val2) TRAP_RET_ZERO
int _write(int val0, char * val1, int val2) TRAP_RET_ZERO
