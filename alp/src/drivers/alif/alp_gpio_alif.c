/**
 ********************************************************************************
 * @file    alp_gpio_alif.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP GPIO Driver - Alif Implementation
 * @note    Wraps ARM CMSIS GPIO driver from Alif SDK
 ********************************************************************************
 */

#include "alp_gpio_vft.h"
#include "Driver_IO.h"
#include <stdio.h>
#include <string.h>

/************************************
 * PRIVATE MACROS
 ************************************/
#define ALIF_GPIO_MAX_PORTS 16
#define ALIF_GPIO_VERBOSE 1

// Helper macros for CMSIS driver access
#define _GPIO_DRIVER_(n) Driver_GPIO##n
#define GPIO_DRIVER_(n) _GPIO_DRIVER_(n)

/************************************
 * PRIVATE TYPES
 ************************************/

typedef struct {
    ARM_DRIVER_GPIO *cmsis_driver;
    uint8_t port;
    uint8_t pin;
    alp_gpio_config_t config;
    bool initialized;
} alif_gpio_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/

// External CMSIS GPIO drivers (from Alif SDK)
extern ARM_DRIVER_GPIO Driver_GPIO0;
extern ARM_DRIVER_GPIO Driver_GPIO1;
extern ARM_DRIVER_GPIO Driver_GPIO2;
extern ARM_DRIVER_GPIO Driver_GPIO3;
extern ARM_DRIVER_GPIO Driver_GPIO4;
extern ARM_DRIVER_GPIO Driver_GPIO13;

static alif_gpio_hw_t g_alif_gpio[ALIF_GPIO_MAX_PORTS] = {0};

/************************************
 * STATIC FUNCTIONS
 ************************************/

static ARM_DRIVER_GPIO* get_cmsis_driver(uint8_t port)
{
    switch (port) {
        case 0: return &Driver_GPIO0;
        case 1: return &Driver_GPIO1;
        case 2: return &Driver_GPIO2;
        case 3: return &Driver_GPIO3;
        case 4: return &Driver_GPIO4;
        case 13: return &Driver_GPIO13;
        default: return NULL;
    }
}

static alp_gpio_status_t alif_init(void *hw, const alp_gpio_config_t *cfg, alp_gpio_event_cb_t event_cb)
{
    (void)event_cb;
    
    if (!hw || !cfg) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    // Get CMSIS driver
    alif->cmsis_driver = get_cmsis_driver(cfg->port);
    if (!alif->cmsis_driver) {
        printf("[ALIF GPIO%d.%d] ERROR: Invalid port\n", cfg->port, cfg->pin);
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
#if ALIF_GPIO_VERBOSE
    printf("\n[ALIF GPIO%d.%d] Initializing\n", cfg->port, cfg->pin);
#endif
    
    // Initialize CMSIS driver (CMSIS 5.x API)
    int32_t ret = alif->cmsis_driver->Initialize(cfg->pin, NULL);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF GPIO%d.%d] ERROR: Initialize failed\n", cfg->port, cfg->pin);
        return ALP_GPIO_ERROR;
    }
    
    // Power on
    ret = alif->cmsis_driver->PowerControl(cfg->pin, ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF GPIO%d.%d] ERROR: PowerControl failed\n", cfg->port, cfg->pin);
        return ALP_GPIO_ERROR;
    }
    
    // Set direction
    ret = alif->cmsis_driver->SetDirection(cfg->pin, 
        cfg->direction == ALP_GPIO_DIRECTION_OUTPUT ? GPIO_PIN_DIRECTION_OUTPUT : GPIO_PIN_DIRECTION_INPUT);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF GPIO%d.%d] ERROR: SetDirection failed\n", cfg->port, cfg->pin);
        return ALP_GPIO_ERROR;
    }
    
    // Store configuration
    memcpy(&alif->config, cfg, sizeof(alp_gpio_config_t));
    alif->port = cfg->port;
    alif->pin = cfg->pin;
    alif->initialized = true;
    
#if ALIF_GPIO_VERBOSE
    printf("  Direction: %s\n", cfg->direction == ALP_GPIO_DIRECTION_OUTPUT ? "OUTPUT" : "INPUT");
    printf("  ✅ Initialized\n\n");
#endif
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t alif_deinit(void *hw)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
#if ALIF_GPIO_VERBOSE
    printf("[ALIF GPIO%d.%d] Deinitializing\n", alif->port, alif->pin);
#endif
    
    alif->cmsis_driver->PowerControl(alif->pin, ARM_POWER_OFF);
    alif->cmsis_driver->Uninitialize(alif->pin);
    
    alif->initialized = false;
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t alif_set_direction(void *hw, alp_gpio_direction_t direction)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    int32_t ret = alif->cmsis_driver->SetDirection(alif->pin,
        direction == ALP_GPIO_DIRECTION_OUTPUT ? GPIO_PIN_DIRECTION_OUTPUT : GPIO_PIN_DIRECTION_INPUT);
    
    if (ret != ARM_DRIVER_OK) {
        return ALP_GPIO_ERROR;
    }
    
    alif->config.direction = direction;
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t alif_write(void *hw, alp_gpio_value_t value)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    int32_t ret = alif->cmsis_driver->SetValue(alif->pin,
        value == ALP_GPIO_VALUE_HIGH ? GPIO_PIN_OUTPUT_STATE_HIGH : GPIO_PIN_OUTPUT_STATE_LOW);
    
    if (ret != ARM_DRIVER_OK) {
        return ALP_GPIO_ERROR;
    }
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t alif_read(void *hw, alp_gpio_value_t *value)
{
    if (!hw || !value) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    uint32_t pin_value;
    int32_t ret = alif->cmsis_driver->GetValue(alif->pin, &pin_value);
    if (ret != ARM_DRIVER_OK) {
        return ALP_GPIO_ERROR;
    }
    
    *value = (pin_value != 0) ? ALP_GPIO_VALUE_HIGH : ALP_GPIO_VALUE_LOW;
    
    return ALP_GPIO_OK;
}

static alp_gpio_status_t alif_toggle(void *hw)
{
    if (!hw) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    alif_gpio_hw_t *alif = (alif_gpio_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_GPIO_ERROR_NOT_INITIALIZED;
    }
    
    int32_t ret = alif->cmsis_driver->SetValue(alif->pin, GPIO_PIN_OUTPUT_STATE_TOGGLE);
    
    if (ret != ARM_DRIVER_OK) {
        return ALP_GPIO_ERROR;
    }
    
    return ALP_GPIO_OK;
}

/************************************
 * GPIO OPERATIONS TABLE
 ************************************/

static const alp_gpio_ops_t alif_gpio_ops = {
    .init = alif_init,
    .deinit = alif_deinit,
    .set_direction = alif_set_direction,
    .write = alif_write,
    .read = alif_read,
    .toggle = alif_toggle
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_gpio_handle_t* alp_gpio_create_alif(uint8_t port, uint8_t pin)
{
    // Find free slot
    uint8_t slot = 0;
    for (slot = 0; slot < ALIF_GPIO_MAX_PORTS; slot++) {
        if (!g_alif_gpio[slot].initialized) {
            break;
        }
    }
    
    if (slot >= ALIF_GPIO_MAX_PORTS) {
        printf("[ALIF GPIO] ERROR: Maximum GPIO pins reached\n");
        return NULL;
    }
    
    // Allocate handle
    alp_gpio_handle_t *handle = (alp_gpio_handle_t*)malloc(sizeof(alp_gpio_handle_t));
    if (!handle) {
        printf("[ALIF GPIO] ERROR: Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_gpio_handle_t));
    
    // Assign VFT
    handle->ops = &alif_gpio_ops;
    
    // Assign hardware handle
    handle->hw_handle = &g_alif_gpio[slot];
    
    // Set platform name
    handle->platform_name = "alif";
    
    // Set port/pin in config
    handle->config.port = port;
    handle->config.pin = pin;
    
    printf("\n[ALIF GPIO%d.%d] Created (Alif Ensemble CMSIS Driver)\n", port, pin);
    printf("  Platform: %s\n", handle->platform_name);
    printf("  Hardware: Alif E7\n\n");
    
    return handle;
}
