/**
 ********************************************************************************
 * @file    alp_spi_core.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   SPI Core - Dispatcher (Platform-independent)
 * @note    This implements the VFT dispatch mechanism
 ********************************************************************************
 */

/************************************
 * INCLUDES
 ************************************/
#include "alp_spi_vft.h"
#include <stdlib.h>
#include <string.h>

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
// None

/************************************
 * PRIVATE TYPEDEFS
 ************************************/
// None

/************************************
 * STATIC VARIABLES
 ************************************/
// None

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
// None

/************************************
 * GLOBAL FUNCTIONS
 ************************************/

/**
 * @brief Destroy SPI handle and free resources
 */
alp_status_t alp_spi_destroy(alp_spi_handle_t *handle)
{
    if (!handle) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    // Deinitialize if initialized
    if (handle->initialized && handle->ops && handle->ops->deinit) {
        handle->ops->deinit(handle->hw_handle);
    }
    
    // Free hardware handle (platform-specific cleanup is done in deinit)
    if (handle->hw_handle) {
        free(handle->hw_handle);
    }
    
    // Free handle itself
    free(handle);
    
    return ALP_STATUS_OK;
}

/**
 * @brief Initialize SPI with given configuration
 * This is the dispatcher function - it calls the platform-specific init via VFT
 */
alp_status_t alp_spi_init(alp_spi_handle_t *handle, const alp_spi_config_t *config, alp_spi_event_cb_t event_cb)
{
    if (!handle || !config) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->ops || !handle->ops->init) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Store configuration
    memcpy(&handle->config, config, sizeof(alp_spi_config_t));
    
    // Call platform-specific init via VFT (Virtual call!)
    alp_status_t ret = handle->ops->init(handle->hw_handle, config, event_cb);
    
    if (ret == ALP_STATUS_OK) {
        handle->initialized = true;
    }
    
    return ret;
}

/**
 * @brief Deinitialize SPI
 */
alp_status_t alp_spi_deinit(alp_spi_handle_t *handle)
{
    if (!handle) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->deinit) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Call platform-specific deinit via VFT
    alp_status_t ret = handle->ops->deinit(handle->hw_handle);
    
    if (ret == ALP_STATUS_OK) {
        handle->initialized = false;
    }
    
    return ret;
}

/**
 * @brief Send data via SPI
 */
alp_status_t alp_spi_send(alp_spi_handle_t *handle, const void *data, uint32_t len)
{
    if (!handle || !data) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->send) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Virtual call - runtime dispatch to correct platform
    return handle->ops->send(handle->hw_handle, data, len);
}

/**
 * @brief Receive data via SPI
 */
alp_status_t alp_spi_receive(alp_spi_handle_t *handle, void *data, uint32_t len)
{
    if (!handle || !data) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->receive) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Virtual call
    return handle->ops->receive(handle->hw_handle, data, len);
}

/**
 * @brief Full-duplex transfer
 */
alp_status_t alp_spi_transfer(alp_spi_handle_t *handle, const void *tx_data, void *rx_data, uint32_t len)
{
    if (!handle || !tx_data || !rx_data) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->transfer) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Virtual call
    return handle->ops->transfer(handle->hw_handle, tx_data, rx_data, len);
}

/**
 * @brief Control SPI
 */
alp_status_t alp_spi_control(alp_spi_handle_t *handle, uint32_t cmd, uint32_t arg)
{
    if (!handle) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->control) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Virtual call
    return handle->ops->control(handle->hw_handle, cmd, arg);
}

/**
 * @brief Get SPI status
 */
alp_status_t alp_spi_get_status(alp_spi_handle_t *handle, alp_spi_status_t *status)
{
    if (!handle || !status) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    if (!handle->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (!handle->ops || !handle->ops->get_status) {
        return ALP_STATUS_ERROR_UNSUPPORTED;
    }
    
    // Virtual call
    return handle->ops->get_status(handle->hw_handle, status);
}

/**
 * @brief Check if SPI is busy
 */
bool alp_spi_is_busy(alp_spi_handle_t *handle)
{
    if (!handle || !handle->initialized) {
        return false;
    }
    
    if (!handle->ops || !handle->ops->is_busy) {
        return false;
    }
    
    // Virtual call
    return handle->ops->is_busy(handle->hw_handle);
}
