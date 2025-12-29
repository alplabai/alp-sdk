/**
 ********************************************************************************
 * @file    alp_usart_alif.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP USART Driver - Alif Implementation
 * @note    Wraps ARM CMSIS USART driver from Alif SDK
 ********************************************************************************
 */

#include "alp_usart_vft.h"
#include "Driver_USART.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/************************************
 * PRIVATE MACROS
 ************************************/
#define ALIF_USART_MAX_INSTANCES 8
#define ALIF_USART_VERBOSE 1

// Helper macros for CMSIS driver access
#define _USART_DRIVER_(n) Driver_USART##n
#define USART_DRIVER_(n) _USART_DRIVER_(n)

/************************************
 * PRIVATE TYPES
 ************************************/

typedef struct {
    ARM_DRIVER_USART *cmsis_driver;
    uint8_t instance;
    alp_usart_config_t config;
    bool initialized;
    size_t tx_count;
    size_t rx_count;
} alif_usart_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/

// External CMSIS USART drivers (from Alif SDK)
extern ARM_DRIVER_USART Driver_USART0;
extern ARM_DRIVER_USART Driver_USART1;
extern ARM_DRIVER_USART Driver_USART2;
extern ARM_DRIVER_USART Driver_USART3;
extern ARM_DRIVER_USART Driver_USART4;
extern ARM_DRIVER_USART Driver_USART5;
extern ARM_DRIVER_USART Driver_USART6;
extern ARM_DRIVER_USART Driver_USART7;

static alif_usart_hw_t g_alif_usart[ALIF_USART_MAX_INSTANCES] = {0};

/************************************
 * STATIC FUNCTIONS
 ************************************/

static ARM_DRIVER_USART* get_cmsis_driver(uint8_t instance)
{
    switch (instance) {
        case 0: return &Driver_USART0;
        case 1: return &Driver_USART1;
        case 2: return &Driver_USART2;
        case 3: return &Driver_USART3;
        case 4: return &Driver_USART4;
        case 5: return &Driver_USART5;
        case 6: return &Driver_USART6;
        case 7: return &Driver_USART7;
        default: return NULL;
    }
}

static void usart_event_callback(uint32_t event)
{
    // Handle CMSIS USART events
    (void)event;
}

static alp_usart_status_t alif_init(void *hw, const alp_usart_config_t *cfg, alp_usart_event_cb_t event_cb)
{
    (void)event_cb;
    
    if (!hw || !cfg) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    // Get CMSIS driver
    alif->cmsis_driver = get_cmsis_driver(cfg->instance);
    if (!alif->cmsis_driver) {
        printf("[ALIF USART%d] ERROR: Invalid instance\n", cfg->instance);
        return ALP_USART_ERROR_PARAMETER;
    }
    
#if ALIF_USART_VERBOSE
    printf("\n[ALIF USART%d] Initializing\n", cfg->instance);
    printf("  Baudrate: %d\n", cfg->baudrate);
#endif
    
    // Initialize CMSIS driver
    int32_t ret = alif->cmsis_driver->Initialize(usart_event_callback);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF USART%d] ERROR: Initialize failed\n", cfg->instance);
        return ALP_USART_ERROR;
    }
    
    // Power on
    ret = alif->cmsis_driver->PowerControl(ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF USART%d] ERROR: PowerControl failed\n", cfg->instance);
        return ALP_USART_ERROR;
    }
    
    // Configure USART
    uint32_t mode = ARM_USART_MODE_ASYNCHRONOUS;
    
    // Data bits
    switch (cfg->data_bits) {
        case ALP_USART_DATA_BITS_5: mode |= ARM_USART_DATA_BITS_5; break;
        case ALP_USART_DATA_BITS_6: mode |= ARM_USART_DATA_BITS_6; break;
        case ALP_USART_DATA_BITS_7: mode |= ARM_USART_DATA_BITS_7; break;
        case ALP_USART_DATA_BITS_8: mode |= ARM_USART_DATA_BITS_8; break;
        case ALP_USART_DATA_BITS_9: mode |= ARM_USART_DATA_BITS_9; break;
    }
    
    // Parity
    switch (cfg->parity) {
        case ALP_USART_PARITY_NONE: mode |= ARM_USART_PARITY_NONE; break;
        case ALP_USART_PARITY_EVEN: mode |= ARM_USART_PARITY_EVEN; break;
        case ALP_USART_PARITY_ODD: mode |= ARM_USART_PARITY_ODD; break;
    }
    
    // Stop bits
    switch (cfg->stop_bits) {
        case ALP_USART_STOP_BITS_1: mode |= ARM_USART_STOP_BITS_1; break;
        case ALP_USART_STOP_BITS_2: mode |= ARM_USART_STOP_BITS_2; break;
    }
    
    // Flow control
    switch (cfg->flow_control) {
        case ALP_USART_FLOW_CONTROL_NONE: mode |= ARM_USART_FLOW_CONTROL_NONE; break;
        case ALP_USART_FLOW_CONTROL_RTS: mode |= ARM_USART_FLOW_CONTROL_RTS; break;
        case ALP_USART_FLOW_CONTROL_CTS: mode |= ARM_USART_FLOW_CONTROL_CTS; break;
        case ALP_USART_FLOW_CONTROL_RTS_CTS: mode |= ARM_USART_FLOW_CONTROL_RTS_CTS; break;
    }
    
    ret = alif->cmsis_driver->Control(mode, cfg->baudrate);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF USART%d] ERROR: Control failed\n", cfg->instance);
        return ALP_USART_ERROR;
    }
    
    // Enable TX and RX
    ret = alif->cmsis_driver->Control(ARM_USART_CONTROL_TX, 1);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF USART%d] ERROR: Enable TX failed\n", cfg->instance);
        return ALP_USART_ERROR;
    }
    
    ret = alif->cmsis_driver->Control(ARM_USART_CONTROL_RX, 1);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF USART%d] ERROR: Enable RX failed\n", cfg->instance);
        return ALP_USART_ERROR;
    }
    
    // Store configuration
    memcpy(&alif->config, cfg, sizeof(alp_usart_config_t));
    alif->instance = cfg->instance;
    alif->tx_count = 0;
    alif->rx_count = 0;
    alif->initialized = true;
    
#if ALIF_USART_VERBOSE
    printf("  ✅ Initialized\n\n");
#endif
    
    return ALP_USART_OK;
}

static alp_usart_status_t alif_deinit(void *hw)
{
    if (!hw) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
#if ALIF_USART_VERBOSE
    printf("[ALIF USART%d] Deinitializing\n", alif->instance);
#endif
    
    alif->cmsis_driver->PowerControl(ARM_POWER_OFF);
    alif->cmsis_driver->Uninitialize();
    
    alif->initialized = false;
    
    return ALP_USART_OK;
}

static alp_usart_status_t alif_send(void *hw, const uint8_t *data, size_t size)
{
    if (!hw || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    int32_t ret = alif->cmsis_driver->Send(data, (uint32_t)size);
    if (ret != ARM_DRIVER_OK) {
        return ALP_USART_ERROR;
    }
    
    // Wait for send complete
    while (alif->cmsis_driver->GetStatus().tx_busy);
    
    alif->tx_count += size;
    
    return ALP_USART_OK;
}

static alp_usart_status_t alif_receive(void *hw, uint8_t *data, size_t size, uint32_t timeout_ms)
{
    (void)timeout_ms;
    
    if (!hw || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    int32_t ret = alif->cmsis_driver->Receive(data, (uint32_t)size);
    if (ret != ARM_DRIVER_OK) {
        return ALP_USART_ERROR;
    }
    
    // Wait for receive complete (simplified, should use timeout)
    while (alif->cmsis_driver->GetStatus().rx_busy);
    
    alif->rx_count += size;
    
    return ALP_USART_OK;
}

static alp_usart_status_t alif_get_tx_count(void *hw, size_t *count)
{
    if (!hw || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    *count = alif->tx_count;
    
    return ALP_USART_OK;
}

static alp_usart_status_t alif_get_rx_count(void *hw, size_t *count)
{
    if (!hw || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    alif_usart_hw_t *alif = (alif_usart_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    *count = alif->rx_count;
    
    return ALP_USART_OK;
}

/************************************
 * USART OPERATIONS TABLE
 ************************************/

static const alp_usart_ops_t alif_usart_ops = {
    .init = alif_init,
    .deinit = alif_deinit,
    .send = alif_send,
    .receive = alif_receive,
    .get_tx_count = alif_get_tx_count,
    .get_rx_count = alif_get_rx_count
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_usart_handle_t* alp_usart_create_alif(uint8_t instance)
{
    if (instance >= ALIF_USART_MAX_INSTANCES) {
        printf("[ALIF USART] ERROR: Invalid instance %d\n", instance);
        return NULL;
    }
    
    // Allocate handle
    alp_usart_handle_t *handle = (alp_usart_handle_t*)malloc(sizeof(alp_usart_handle_t));
    if (!handle) {
        printf("[ALIF USART] ERROR: Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_usart_handle_t));
    
    // Assign VFT
    handle->ops = &alif_usart_ops;
    
    // Assign hardware handle
    handle->hw_handle = &g_alif_usart[instance];
    
    // Set platform name
    handle->platform_name = "alif";
    
    printf("\n[ALIF USART%d] Created (Alif Ensemble CMSIS Driver)\n", instance);
    printf("  Platform: %s\n", handle->platform_name);
    printf("  Hardware: Alif E7\n\n");
    
    return handle;
}
