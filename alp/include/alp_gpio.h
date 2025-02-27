/**
 ********************************************************************************
 * @file    alp_gpio.h
 * @author  Gurkan Kucukyildiz
 * @date    31/10/2024
 * @brief   Header file for ALP GPIO configuration functions
 ********************************************************************************
 */

#ifndef ALP_GPIO_H
#define ALP_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include "stdint.h"

/************************************
 * MACROS AND DEFINES
 ************************************/
// Define GPIO directions if needed, e.g.,
// #define GPIO_DIRECTION_INPUT   0
// #define GPIO_DIRECTION_OUTPUT  1

/************************************
 * TYPEDEFS
 ************************************/
/* Enumeration for GPIO Names */
typedef enum
{
    GPIO1,
    GPIO2,
    GPIO3,
    GPIO4
} GPIOName;

/************************************
 * EXPORTED VARIABLES
 ************************************/
// Add any extern variable declarations if necessary

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
/**
 * @brief Sets the direction of a specified GPIO pin.
 * @param Port      GPIO port number.
 * @param pin       GPIO pin number.
 * @param direction Direction to set (1 for output, 0 for input).
 */
void ALP_GPIO_SetDirection(uint8_t Port, uint8_t pin, uint8_t direction);

/**
 * @brief Gets the direction of a specified GPIO pin.
 * @param Port      GPIO port number.
 * @param pin       GPIO pin number.
 * @param direction Variable to store the direction (1 for output, 0 for input).
 */
void ALP_GPIO_GetDirection(uint8_t Port, uint8_t pin, uint8_t direction);

#ifdef __cplusplus
}
#endif

#endif /* ALP_GPIO_H */
