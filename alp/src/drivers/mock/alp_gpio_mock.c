/**
 ********************************************************************************
 * @file    alp_gpio_mock.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP GPIO Driver - Mock Implementation
 * @note    Simulates GPIO in memory - NO HARDWARE REQUIRED
 ********************************************************************************
 */

#include "alp_gpio_vft.h"
#include <stdio.h>
#include <string.h>

/************************************
 * PRIVATE TYPES
 ************************************/

/**
 * @brief Mock GPIO Hardware State
 */
typedef struct {
    uint8_t port;
    uint8_t pin;
    alp_gpio_direction_t direction;
    alp_gpio_value_t value;
    bool initialized;
} mock_gpio_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/

#define MAX_MOCK_GPIO_PINS 16
static mock_gpio_hw_t g_mock_gpio[MAX_MOCK_GPIO_PINS] = {0};
static uint8_t g_mock_gpio_count = 0;

/************************************
 * STATIC FUNCTIONS
 ************************************/

static alp_gpio_status_t mock_init(void *hw, const alp_gpio_config_t *cfg, alp_gpio_event_cb_t event_cb)
{
    (void)event_cb;
    
    if (!hw || !cfg) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    printf("[MOCK GPIO%d.%d] Initializing\n", cfg->port, cfg->pin);
    printf("  Direction: %s\n", cfg->direction == ALP_GPIO_DIRECTION_OUTPUT ? "OUTPUT" : "INPUT");
    printf("  Pull: ");
    switch (cfg->pull) {
        case ALP_GPIO_PULL_UP: printf("PULL_UP\n"); break;
        case ALP_GPIO_PULL_DOWN: printf("PULL_DOWN\n"); break;
        default: printf("NONE\n"); break;
    }
    
    mock->port = cfg->port;
    mock->pin = cfg->pin;
    mock->direction = cfg->direction;
    mock->value = ALP_GPIO_VALUE_LOW;
    mock->initialized = true;
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t mock_deinit(void *hw)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    printf("[MOCK GPIO%d.%d] Deinitializing\n", mock->port, mock->pin);
    
    mock->initialized = false;
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t mock_set_direction(void *hw, alp_gpio_direction_t direction)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    mock->direction = direction;
    
    printf("[MOCK GPIO%d.%d] Direction set to %s\n", 
           mock->port, mock->pin,
           direction == ALP_GPIO_DIRECTION_OUTPUT ? "OUTPUT" : "INPUT");
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t mock_write(void *hw, alp_gpio_value_t value)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    if (mock->direction != ALP_GPIO_DIRECTION_OUTPUT) {
        printf("[MOCK GPIO%d.%d] ERROR: Cannot write to input pin\n", mock->port, mock->pin);
        return ALP_GPIO_ERROR;
    }
    
    mock->value = value;
    
    printf("[MOCK GPIO%d.%d] Write: %s\n", 
           mock->port, mock->pin,
           value == ALP_GPIO_VALUE_HIGH ? "HIGH" : "LOW");
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t mock_read(void *hw, alp_gpio_value_t *value)
{
    if (!hw || !value) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    *value = mock->value;
    
    printf("[MOCK GPIO%d.%d] Read: %s\n", 
           mock->port, mock->pin,
           *value == ALP_GPIO_VALUE_HIGH ? "HIGH" : "LOW");
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t mock_toggle(void *hw)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    mock_gpio_hw_t *mock = (mock_gpio_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    if (mock->direction != ALP_GPIO_DIRECTION_OUTPUT) {
        printf("[MOCK GPIO%d.%d] ERROR: Cannot toggle input pin\n", mock->port, mock->pin);
        return ALP_GPIO_ERROR;
    }
    
    mock->value = (mock->value == ALP_GPIO_VALUE_HIGH) ? ALP_GPIO_VALUE_LOW : ALP_GPIO_VALUE_HIGH;
    
    printf("[MOCK GPIO%d.%d] Toggle: %s\n", 
           mock->port, mock->pin,
           mock->value == ALP_GPIO_VALUE_HIGH ? "HIGH" : "LOW");
    
    return ALP_GPIO_OK;
}

/************************************
 * GPIO OPERATIONS TABLE
 ************************************/

static const alp_gpio_ops_t mock_gpio_ops = {
    .init = mock_init,
    .deinit = mock_deinit,
    .set_direction = mock_set_direction,
    .write = mock_write,
    .read = mock_read,
    .toggle = mock_toggle
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_gpio_handle_t* alp_gpio_create_mock(uint8_t port, uint8_t pin)
{
    if (g_mock_gpio_count >= MAX_MOCK_GPIO_PINS) {
        printf("[MOCK GPIO] ERROR: Maximum GPIO pins reached\n");
        return NULL;
    }
    
    // Allocate handle
    alp_gpio_handle_t *handle = (alp_gpio_handle_t*)malloc(sizeof(alp_gpio_handle_t));
    if (!handle) {
        printf("[MOCK GPIO] ERROR: Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_gpio_handle_t));
    
    // Assign VFT
    handle->ops = &mock_gpio_ops;
    
    // Assign hardware handle
    handle->hw_handle = &g_mock_gpio[g_mock_gpio_count];
    g_mock_gpio_count++;
    
    // Set platform name
    handle->platform_name = "mock";
    
    // Set port/pin in config
    handle->config.port = port;
    handle->config.pin = pin;
    
    printf("\n[MOCK GPIO%d.%d] Created (NO HARDWARE REQUIRED)\n", port, pin);
    printf("  Platform: %s\n", handle->platform_name);
    printf("  Hardware: Simulated\n\n");
    
    return handle;
}
