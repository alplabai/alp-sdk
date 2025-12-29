/**
 ********************************************************************************
 * @file    alp_usart_core.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP USART Driver - Core Dispatcher
 * @note    Platform-independent dispatcher routing through VFT
 ********************************************************************************
 */

#include "alp_usart_vft.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_usart_status_t alp_usart_init(alp_usart_handle_t *handle, 
                                   const alp_usart_config_t *cfg,
                                   alp_usart_event_cb_t event_cb)
{
    if (!handle || !handle->ops || !cfg) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->init) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->init(handle->hw_handle, cfg, event_cb);
}

alp_usart_status_t alp_usart_deinit(alp_usart_handle_t *handle)
{
    if (!handle || !handle->ops) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->deinit) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->deinit(handle->hw_handle);
}

alp_usart_status_t alp_usart_send(alp_usart_handle_t *handle, 
                                   const uint8_t *data, 
                                   size_t size)
{
    if (!handle || !handle->ops || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->send) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->send(handle->hw_handle, data, size);
}

alp_usart_status_t alp_usart_receive(alp_usart_handle_t *handle, 
                                      uint8_t *data, 
                                      size_t size,
                                      uint32_t timeout_ms)
{
    if (!handle || !handle->ops || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->receive) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->receive(handle->hw_handle, data, size, timeout_ms);
}

alp_usart_status_t alp_usart_get_tx_count(alp_usart_handle_t *handle, size_t *count)
{
    if (!handle || !handle->ops || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->get_tx_count) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->get_tx_count(handle->hw_handle, count);
}

alp_usart_status_t alp_usart_get_rx_count(alp_usart_handle_t *handle, size_t *count)
{
    if (!handle || !handle->ops || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    if (!handle->ops->get_rx_count) {
        return ALP_USART_ERROR;
    }
    
    return handle->ops->get_rx_count(handle->hw_handle, count);
}

int alp_usart_printf(alp_usart_handle_t *handle, const char *format, ...)
{
    if (!handle || !format) {
        return -1;
    }
    
    char buffer[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    if (len < 0) {
        return -1;
    }
    
    if (len >= (int)sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    
    alp_usart_status_t status = alp_usart_send(handle, (const uint8_t*)buffer, (size_t)len);
    
    return (status == ALP_USART_OK) ? len : -1;
}
