/**
 ********************************************************************************
 * @file    alp_board.h
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP Board Support Package - Common Interface
 * @note    Platform-independent board configuration
 ********************************************************************************
 */

#ifndef ALP_BOARD_H
#define ALP_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include <stdint.h>
#include <stdbool.h>

/************************************
 * BOARD IDENTIFICATION
 ************************************/

typedef enum {
    ALP_BOARD_MOCK = 0,
    ALP_BOARD_ALIF_E7_DEVKIT = 1,
    ALP_BOARD_ALIF_E1M_CUSTOM = 2,
    ALP_BOARD_RENESAS_FUTURE = 3
} alp_board_type_t;

/************************************
 * LED DEFINITIONS
 ************************************/

typedef struct {
    uint8_t port;
    uint8_t pin;
} alp_board_pin_t;

typedef struct {
    alp_board_pin_t led1;
    alp_board_pin_t led2;
    alp_board_pin_t button1;
    alp_board_pin_t button2;
} alp_board_gpio_t;

typedef struct {
    uint8_t instance;
    uint32_t baudrate;
} alp_board_usart_t;

/************************************
 * BOARD CONFIGURATION
 ************************************/

typedef struct {
    const char *name;
    alp_board_type_t type;
    alp_board_gpio_t gpio;
    alp_board_usart_t usart;
} alp_board_config_t;

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

/**
 * @brief Get board configuration
 * @return Pointer to board configuration structure
 */
const alp_board_config_t* alp_board_get_config(void);

/**
 * @brief Initialize board (LEDs, buttons, USART)
 * @return true on success, false on failure
 */
bool alp_board_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_BOARD_H */
