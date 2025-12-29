/**
 ********************************************************************************
 * @file    alp_usart_mock.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP USART Driver - Mock Implementation
 * @note    Simulates USART communication (console output)
 ********************************************************************************
 */

#include "alp_usart_vft.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/************************************
 * PRIVATE MACROS
 ************************************/
#define MAX_MOCK_USART_INSTANCES 4
#define MOCK_RX_BUFFER_SIZE 256
#define MOCK_USART_VERBOSE 1

/************************************
 * PRIVATE TYPES
 ************************************/

typedef struct {
    uint8_t instance;
    alp_usart_config_t config;
    bool initialized;
    size_t tx_count;
    size_t rx_count;
    uint8_t rx_buffer[MOCK_RX_BUFFER_SIZE];
    size_t rx_head;
    size_t rx_tail;
} mock_usart_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/

static mock_usart_hw_t g_mock_usart[MAX_MOCK_USART_INSTANCES] = {0};

/************************************
 * STATIC FUNCTIONS
 ************************************/

static alp_usart_status_t mock_init(void *hw, const alp_usart_config_t *cfg, alp_usart_event_cb_t event_cb)
{
    (void)event_cb;
    
    if (!hw || !cfg) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
#if MOCK_USART_VERBOSE
    printf("\n[MOCK USART%d] Initializing\n", cfg->instance);
    printf("  Baudrate: %d\n", cfg->baudrate);
    printf("  Data bits: %d\n", cfg->data_bits);
    printf("  Parity: %s\n", 
        cfg->parity == ALP_USART_PARITY_NONE ? "None" :
        cfg->parity == ALP_USART_PARITY_EVEN ? "Even" : "Odd");
    printf("  Stop bits: %d\n", cfg->stop_bits);
    printf("  ✅ Initialized\n\n");
#endif
    
    memcpy(&mock->config, cfg, sizeof(alp_usart_config_t));
    mock->instance = cfg->instance;
    mock->tx_count = 0;
    mock->rx_count = 0;
    mock->rx_head = 0;
    mock->rx_tail = 0;
    mock->initialized = true;
    
    return ALP_USART_OK;
}

static alp_usart_status_t mock_deinit(void *hw)
{
    if (!hw) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
#if MOCK_USART_VERBOSE
    printf("[MOCK USART%d] Deinitializing\n", mock->instance);
#endif
    
    mock->initialized = false;
    
    return ALP_USART_OK;
}

static alp_usart_status_t mock_send(void *hw, const uint8_t *data, size_t size)
{
    if (!hw || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
#if MOCK_USART_VERBOSE
    printf("[MOCK USART%d] TX: ", mock->instance);
    for (size_t i = 0; i < size; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            printf("%c", data[i]);
        } else {
            printf("\\x%02X", data[i]);
        }
    }
    printf("\n");
#endif
    
    mock->tx_count += size;
    
    return ALP_USART_OK;
}

static alp_usart_status_t mock_receive(void *hw, uint8_t *data, size_t size, uint32_t timeout_ms)
{
    (void)timeout_ms;
    
    if (!hw || !data || size == 0) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    // Calculate available bytes in circular buffer
    size_t available = (mock->rx_head >= mock->rx_tail) ? 
        (mock->rx_head - mock->rx_tail) : 
        (MOCK_RX_BUFFER_SIZE - mock->rx_tail + mock->rx_head);
    
    if (available < size) {
#if MOCK_USART_VERBOSE
        printf("[MOCK USART%d] RX: Not enough data (requested: %zu, available: %zu)\n", 
               mock->instance, size, available);
#endif
        return ALP_USART_ERROR_TIMEOUT;
    }
    
    // Copy data from circular buffer
    for (size_t i = 0; i < size; i++) {
        data[i] = mock->rx_buffer[mock->rx_tail];
        mock->rx_tail = (mock->rx_tail + 1) % MOCK_RX_BUFFER_SIZE;
    }
    
    mock->rx_count += size;
    
#if MOCK_USART_VERBOSE
    printf("[MOCK USART%d] RX: ", mock->instance);
    for (size_t i = 0; i < size; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            printf("%c", data[i]);
        } else {
            printf("\\x%02X", data[i]);
        }
    }
    printf("\n");
#endif
    
    return ALP_USART_OK;
}

static alp_usart_status_t mock_get_tx_count(void *hw, size_t *count)
{
    if (!hw || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    *count = mock->tx_count;
    
    return ALP_USART_OK;
}

static alp_usart_status_t mock_get_rx_count(void *hw, size_t *count)
{
    if (!hw || !count) {
        return ALP_USART_ERROR_PARAMETER;
    }
    
    mock_usart_hw_t *mock = (mock_usart_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_USART_ERROR_NOT_INITIALIZED;
    }
    
    *count = mock->rx_count;
    
    return ALP_USART_OK;
}

/************************************
 * USART OPERATIONS TABLE
 ************************************/

static const alp_usart_ops_t mock_usart_ops = {
    .init = mock_init,
    .deinit = mock_deinit,
    .send = mock_send,
    .receive = mock_receive,
    .get_tx_count = mock_get_tx_count,
    .get_rx_count = mock_get_rx_count
};

/************************************
 * PUBLIC FUNCTIONS
 ************************************/

alp_usart_handle_t* alp_usart_create_mock(uint8_t instance)
{
    if (instance >= MAX_MOCK_USART_INSTANCES) {
        printf("[MOCK USART] ERROR: Invalid instance %d\n", instance);
        return NULL;
    }
    
    // Allocate handle
    alp_usart_handle_t *handle = (alp_usart_handle_t*)malloc(sizeof(alp_usart_handle_t));
    if (!handle) {
        printf("[MOCK USART] ERROR: Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_usart_handle_t));
    
    // Assign VFT
    handle->ops = &mock_usart_ops;
    
    // Assign hardware handle
    handle->hw_handle = &g_mock_usart[instance];
    
    // Set platform name
    handle->platform_name = "mock";
    
    printf("\n[MOCK USART%d] Created (Mock USART Driver)\n", instance);
    printf("  Platform: %s\n", handle->platform_name);
    printf("  Hardware: Simulated\n\n");
    
    return handle;
}
