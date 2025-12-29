/**
 ********************************************************************************
 * @file    alp_gpio_vft.h
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP GPIO Driver - Virtual Function Table Interface
 * @note    Platform-independent GPIO API using VFT pattern
 ********************************************************************************
 */

#ifndef ALP_GPIO_VFT_H
#define ALP_GPIO_VFT_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/************************************
 * TYPES AND ENUMS
 ************************************/

/**
 * @brief ALP Status Codes
 */
typedef enum {
    ALP_GPIO_OK = 0,
    ALP_GPIO_ERROR = -1,
    ALP_GPIO_ERROR_PARAMETER = -2,
    ALP_GPIO_ERROR_NOT_INITIALIZED = -3,
    ALP_GPIO_ERROR_BUSY = -4,
    ALP_GPIO_ERROR_TIMEOUT = -5
} alp_gpio_status_t;

/**
 * @brief GPIO Pin Direction
 */
typedef enum {
    ALP_GPIO_DIRECTION_INPUT = 0,
    ALP_GPIO_DIRECTION_OUTPUT = 1
} alp_gpio_direction_t;

/**
 * @brief GPIO Pin Value
 */
typedef enum {
    ALP_GPIO_VALUE_LOW = 0,
    ALP_GPIO_VALUE_HIGH = 1
} alp_gpio_value_t;

/**
 * @brief GPIO Pull Mode
 */
typedef enum {
    ALP_GPIO_PULL_NONE = 0,
    ALP_GPIO_PULL_UP = 1,
    ALP_GPIO_PULL_DOWN = 2
} alp_gpio_pull_t;

/**
 * @brief GPIO Interrupt Mode
 */
typedef enum {
    ALP_GPIO_IRQ_NONE = 0,
    ALP_GPIO_IRQ_RISING_EDGE = 1,
    ALP_GPIO_IRQ_FALLING_EDGE = 2,
    ALP_GPIO_IRQ_BOTH_EDGES = 3
} alp_gpio_irq_mode_t;

/**
 * @brief GPIO Configuration
 */
typedef struct {
    uint8_t port;                    /**< GPIO port number */
    uint8_t pin;                     /**< GPIO pin number */
    alp_gpio_direction_t direction;  /**< Pin direction */
    alp_gpio_pull_t pull;            /**< Pull resistor configuration */
    alp_gpio_irq_mode_t irq_mode;    /**< Interrupt mode */
} alp_gpio_config_t;

/**
 * @brief GPIO Event Callback
 */
typedef void (*alp_gpio_event_cb_t)(uint32_t event);

/************************************
 * FORWARD DECLARATIONS
 ************************************/
struct alp_gpio_handle;
struct alp_gpio_ops;

/**
 * @brief GPIO Virtual Function Table (Operations)
 */
typedef struct alp_gpio_ops {
    alp_gpio_status_t (*init)(void *hw, const alp_gpio_config_t *cfg, alp_gpio_event_cb_t event_cb);
    alp_gpio_status_t (*deinit)(void *hw);
    alp_gpio_status_t (*set_direction)(void *hw, alp_gpio_direction_t direction);
    alp_gpio_status_t (*write)(void *hw, alp_gpio_value_t value);
    alp_gpio_status_t (*read)(void *hw, alp_gpio_value_t *value);
    alp_gpio_status_t (*toggle)(void *hw);
} alp_gpio_ops_t;

/**
 * @brief GPIO Handle Structure
 */
typedef struct alp_gpio_handle {
    const alp_gpio_ops_t *ops;       /**< Virtual function table */
    void *hw_handle;                 /**< Platform-specific hardware handle */
    alp_gpio_config_t config;        /**< GPIO configuration */
    const char *platform_name;       /**< Platform identifier (e.g., "mock", "alif") */
} alp_gpio_handle_t;

/************************************
 * PUBLIC API FUNCTIONS
 ************************************/

/**
 * @brief Initialize GPIO pin
 * @param handle GPIO handle
 * @param config GPIO configuration
 * @param event_cb Event callback (can be NULL)
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_init(alp_gpio_handle_t *handle, 
                                const alp_gpio_config_t *config,
                                alp_gpio_event_cb_t event_cb);

/**
 * @brief Deinitialize GPIO pin
 * @param handle GPIO handle
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_deinit(alp_gpio_handle_t *handle);

/**
 * @brief Set GPIO pin direction
 * @param handle GPIO handle
 * @param direction Input or Output
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_set_direction(alp_gpio_handle_t *handle,
                                         alp_gpio_direction_t direction);

/**
 * @brief Write value to GPIO pin
 * @param handle GPIO handle
 * @param value HIGH or LOW
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_write(alp_gpio_handle_t *handle,
                                 alp_gpio_value_t value);

/**
 * @brief Read value from GPIO pin
 * @param handle GPIO handle
 * @param value Pointer to store read value
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_read(alp_gpio_handle_t *handle,
                                alp_gpio_value_t *value);

/**
 * @brief Toggle GPIO pin
 * @param handle GPIO handle
 * @return ALP_GPIO_OK on success
 */
alp_gpio_status_t alp_gpio_toggle(alp_gpio_handle_t *handle);

/************************************
 * FACTORY FUNCTIONS
 ************************************/

/**
 * @brief Create Mock GPIO handle
 * @param port GPIO port number
 * @param pin GPIO pin number
 * @return Pointer to GPIO handle, or NULL on error
 */
alp_gpio_handle_t* alp_gpio_create_mock(uint8_t port, uint8_t pin);

/**
 * @brief Create Alif GPIO handle
 * @param port GPIO port number
 * @param pin GPIO pin number
 * @return Pointer to GPIO handle, or NULL on error
 */
alp_gpio_handle_t* alp_gpio_create_alif(uint8_t port, uint8_t pin);

#ifdef __cplusplus
}
#endif

#endif // ALP_GPIO_VFT_H
