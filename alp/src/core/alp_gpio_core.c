/**
 ********************************************************************************
 * @file    alp_gpio_core.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP GPIO Driver - Core Dispatcher
 * @note    Platform-independent GPIO dispatcher using VFT pattern
 ********************************************************************************
 */

#include "alp_gpio_vft.h"
#include <string.h>

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_gpio_status_t alp_gpio_init(alp_gpio_handle_t *handle,
                                const alp_gpio_config_t *config,
                                alp_gpio_event_cb_t event_cb)
{
    if (!handle || !config) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->init) {
        return ALP_GPIO_ERROR;
    }
    
    // Store configuration
    memcpy(&handle->config, config, sizeof(alp_gpio_config_t));
    
    // Call platform-specific init through VFT
    return handle->ops->init(handle->hw_handle, config, event_cb);
}

alp_gpio_status_t alp_gpio_deinit(alp_gpio_handle_t *handle)
{
    if (!handle) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->deinit) {
        return ALP_GPIO_ERROR;
    }
    
    return handle->ops->deinit(handle->hw_handle);
}

alp_gpio_status_t alp_gpio_set_direction(alp_gpio_handle_t *handle,
                                         alp_gpio_direction_t direction)
{
    if (!handle) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->set_direction) {
        return ALP_GPIO_ERROR;
    }
    
    return handle->ops->set_direction(handle->hw_handle, direction);
}

alp_gpio_status_t alp_gpio_write(alp_gpio_handle_t *handle,
                                 alp_gpio_value_t value)
{
    if (!handle) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->write) {
        return ALP_GPIO_ERROR;
    }
    
    return handle->ops->write(handle->hw_handle, value);
}

alp_gpio_status_t alp_gpio_read(alp_gpio_handle_t *handle,
                                alp_gpio_value_t *value)
{
    if (!handle || !value) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->read) {
        return ALP_GPIO_ERROR;
    }
    
    return handle->ops->read(handle->hw_handle, value);
}

alp_gpio_status_t alp_gpio_toggle(alp_gpio_handle_t *handle)
{
    if (!handle) {
        return ALP_GPIO_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->toggle) {
        return ALP_GPIO_ERROR;
    }
    
    return handle->ops->toggle(handle->hw_handle);
}
