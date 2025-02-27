/**
 ********************************************************************************
 * @file    alp_gpio.c
 * @author  Gurkan Kucukyildiz
 * @date    31/10/2024
 * @brief   Implementation of ALP GPIO configuration functions
 ********************************************************************************
 */
// TEMPORARY EXPLANATION OF ALIF GPIO DRIVER IMPLEMENTATION

// #define _GET_DRIVER_REF(ref, peri, chan) \
//     extern ARM_DRIVER_##peri Driver_##peri##chan; \
//     static ARM_DRIVER_##peri * ref = &Driver_##peri##chan;
// #define GET_DRIVER_REF(ref, peri, chan) _GET_DRIVER_REF(ref, peri, chan)

// GET_DRIVER_REF(gpio_b, GPIO, BOARD_LEDRGB0_B_GPIO_PORT);
// GET_DRIVER_REF(gpio_r, GPIO, BOARD_LEDRGB0_R_GPIO_PORT);

// expands to:

// extern ARM_DRIVER_GPIO Driver_GPIOBOARD_LEDRGB0_B_GPIO_PORT;
// static ARM_DRIVER_GPIO * gpio_b = &Driver_GPIOBOARD_LEDRGB0_B_GPIO_PORT;

// extern ARM_DRIVER_GPIO Driver_GPIOBOARD_LEDRGB0_R_GPIO_PORT;
// static ARM_DRIVER_GPIO * gpio_r = &Driver_GPIOBOARD_LEDRGB0_R_GPIO_PORT;

// because of these lines on board_defs.h:

// #define BOARD_LEDRGB0_B_GPIO_PORT               12
// #define BOARD_LEDRGB0_R_GPIO_PORT               12

// overall expands to:
// extern ARM_DRIVER_GPIO Driver_GPIO12;
// static ARM_DRIVER_GPIO * gpio_b = &Driver_GPIO12;

// extern ARM_DRIVER_GPIO Driver_GPIO12;
// static ARM_DRIVER_GPIO * gpio_r = &Driver_GPIO12;

// Actually setting the LEDs on blinky/main.c:

// void SysTick_Handler (void)
// {
// #ifdef CORE_M55_HE
//     gpio_b->SetValue(BOARD_LEDRGB0_B_PIN_NO, GPIO_PIN_OUTPUT_STATE_TOGGLE);
// #else
//     gpio_r->SetValue(BOARD_LEDRGB0_R_PIN_NO, GPIO_PIN_OUTPUT_STATE_TOGGLE);
// #endif
// }


// Note that:

// #define BOARD_LEDRGB0_B_PIN_NO                  0
// #define BOARD_LEDRGB0_R_PIN_NO                  3



/************************************
 * INCLUDES
 ************************************/
#include "ALP_GPIO.h"
#include "Driver_GPIO.h"

/************************************
 * EXTERN VARIABLES
 ************************************/
// Add extern variable declarations if necessary
// TODO: we need to define all GPIO instances to access all of them.
extern ARM_DRIVER_GPIO Driver_GPIO12;
/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
// Define any private macros or constants if needed

/************************************
 * PRIVATE TYPEDEFS
 ************************************/
// Define private typedefs if needed

/************************************
 * STATIC VARIABLES
 ************************************/
// Declare static (file-scoped) variables if needed
static ARM_DRIVER_GPIO *gpio_driver_12 = &Driver_GPIO12;
/************************************
 * GLOBAL VARIABLES
 ************************************/
// Declare any global variables if necessary

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
// Declare static (private) function prototypes if needed

/************************************
 * STATIC FUNCTIONS
 ************************************/
// Implement static (private) functions if needed

/************************************
 * GLOBAL FUNCTIONS
 ************************************/

/**
 * @brief Sets the direction of a specified GPIO pin.
 * 
 * @param Port      GPIO port number (e.g., GPIO1, GPIO2, etc.).
 * @param pin       GPIO pin number.
 * @param direction Direction to set: `1` for output, `0` for input.
 */
void ALP_GPIO_SetDirection(uint8_t Port, uint8_t pin, uint8_t direction)
{
   //  switch (Port)
   //  {
   //      case GPIO1:
   //          ARM_GPIO1_SetDirection(pin, direction);
   //          break;
   //      case GPIO2:
   //          ARM_GPIO2_SetDirection(pin, direction);
   //          break;
   //      case GPIO3:
   //          ARM_GPIO3_SetDirection(pin, direction);
   //          break;
   //      case GPIO4:
   //          ARM_GPIO4_SetDirection(pin, direction);
   //          break;
   //      default:
   //          Handle invalid Port values if needed
   //          break;
   //  }
}

/**
 * @brief Gets the direction of a specified GPIO pin.
 * 
 * @param Port      GPIO port number.
 * @param pin       GPIO pin number.
 * @param direction Pointer to store the direction: `1` for output, `0` for input.
 */
void ALP_GPIO_GetDirection(uint8_t Port, uint8_t pin, uint8_t direction)
{
   //  switch (Port)
   //  {
   //      case GPIO1:
   //          ARM_GPIO1_GetDirection(pin, direction);
   //          break;
   //      case GPIO2:
   //          ARM_GPIO2_GetDirection(pin, direction);
   //          break;
   //      case GPIO3:
   //          ARM_GPIO3_GetDirection(pin, direction);
   //          break;
   //      case GPIO4:
   //          ARM_GPIO4_GetDirection(pin, direction);
   //          break;
   //      default:
   //          // Handle invalid Port values if needed
   //          break;
   //  }
}

/**
 * @brief Sets the output value of a specified GPIO pin.
 * 
 * @param Port  GPIO port number.
 * @param pin   GPIO pin number.
 * @param value Output state to set (e.g., HIGH or LOW).
 */
void ALP_GPIO_SetValue(uint8_t Port, uint8_t pin, GPIO_PIN_OUTPUT_STATE value)
{
   //  switch (Port)
   //  {
   //      case GPIO1:
   //          ARM_GPIO1_SetValue(pin, value);
   //          break;
   //      case GPIO2:
   //          ARM_GPIO2_SetValue(pin, value);
   //          break;
   //      case GPIO3:
   //          ARM_GPIO3_SetValue(pin, value);
   //          break;
   //      case GPIO4:
   //          ARM_GPIO4_SetValue(pin, value);
   //          break;
   //      default:
   //          // Handle invalid Port values if needed
   //          break;
   //  }
}

/**
 * @brief Gets the output value of a specified GPIO pin.
 * 
 * @param Port  GPIO port number.
 * @param pin   GPIO pin number.
 * @param value Pointer to store the pin's output state (e.g., HIGH or LOW).
 */
void ALP_GPIO_GetValue(uint8_t Port, uint8_t pin, GPIO_PIN_OUTPUT_STATE* value)
{
   //  switch (Port)
   //  {
   //      case GPIO1:
   //          ARM_GPIO1_GetValue(pin, value);
   //          break;
   //      case GPIO2:
   //          ARM_GPIO2_GetValue(pin, value);
   //          break;
   //      case GPIO3:
   //          ARM_GPIO3_GetValue(pin, value);
   //          break;
   //      case GPIO4:
   //          ARM_GPIO4_GetValue(pin, value);
   //          break;
   //      default:
   //          // Handle invalid Port values if needed
   //          break;
   //  }
}
