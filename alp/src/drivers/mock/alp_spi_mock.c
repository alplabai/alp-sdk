/**
 ********************************************************************************
 * @file    alp_spi_mock.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Mock SPI Driver - NO VENDOR SDK REQUIRED!
 * @note    This driver simulates SPI hardware for development and testing
 *          It allows the SDK to build and run without any vendor dependencies
 ********************************************************************************
 */

/************************************
 * INCLUDES
 ************************************/
#include "alp_spi_vft.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
#define MOCK_SPI_BUFFER_SIZE    256
#define MOCK_SPI_VERBOSE        1   // Enable detailed logging

/************************************
 * PRIVATE TYPEDEFS
 ************************************/

/**
 * @brief Mock SPI Hardware State
 * This simulates the hardware registers and buffers
 */
typedef struct {
    // Configuration
    alp_spi_config_t config;
    alp_spi_event_cb_t event_cb;
    
    // Buffers (simulated hardware FIFOs)
    uint8_t tx_buffer[MOCK_SPI_BUFFER_SIZE];
    uint8_t rx_buffer[MOCK_SPI_BUFFER_SIZE];
    uint32_t tx_count;
    uint32_t rx_count;
    
    // Status
    bool initialized;
    bool busy;
    bool data_lost;
    
    // Statistics
    uint32_t total_tx_bytes;
    uint32_t total_rx_bytes;
    uint32_t total_transfers;
    
} mock_spi_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/
// Global mock hardware instances (up to 4 SPI instances)
static mock_spi_hw_t g_mock_spi[ALP_SPI_MAX_INSTANCES] = {0};

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
static alp_status_t mock_init(void *hw, const alp_spi_config_t *cfg, alp_spi_event_cb_t event_cb);
static alp_status_t mock_deinit(void *hw);
static alp_status_t mock_send(void *hw, const void *data, uint32_t len);
static alp_status_t mock_receive(void *hw, void *data, uint32_t len);
static alp_status_t mock_transfer(void *hw, const void *tx_data, void *rx_data, uint32_t len);
static alp_status_t mock_control(void *hw, uint32_t cmd, uint32_t arg);
static alp_status_t mock_get_status(void *hw, alp_spi_status_t *status);
static bool mock_is_busy(void *hw);

/************************************
 * STATIC FUNCTIONS
 ************************************/

/**
 * @brief Initialize Mock SPI hardware
 */
static alp_status_t mock_init(void *hw, const alp_spi_config_t *cfg, alp_spi_event_cb_t event_cb)
{
    if (!hw || !cfg) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    // Clear hardware state
    memset(mock, 0, sizeof(mock_spi_hw_t));
    
    // Store configuration
    memcpy(&mock->config, cfg, sizeof(alp_spi_config_t));
    mock->event_cb = event_cb;
    mock->initialized = true;
    
#if MOCK_SPI_VERBOSE
    printf("\n[MOCK SPI%d] ✅ Initialized\n", cfg->instance);
    printf("  - Mode: %s\n", 
           cfg->mode == ALP_SPI_MODE_MASTER ? "Master" : "Slave");
    printf("  - Baudrate: %u Hz\n", cfg->baudrate);
    printf("  - Data bits: %u\n", cfg->data_bits);
    printf("  - Clock mode: CPOL%d_CPHA%d\n", 
           (cfg->clock_mode >> 1) & 1, cfg->clock_mode & 1);
    printf("  - Bit order: %s\n", 
           cfg->bit_order == ALP_SPI_MSB_FIRST ? "MSB first" : "LSB first");
#endif
    
    return ALP_STATUS_OK;
}

/**
 * @brief Deinitialize Mock SPI hardware
 */
static alp_status_t mock_deinit(void *hw)
{
    if (!hw) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
#if MOCK_SPI_VERBOSE
    printf("\n[MOCK SPI%d] 🔌 Deinitialized\n", mock->config.instance);
    printf("  - Total TX: %u bytes\n", mock->total_tx_bytes);
    printf("  - Total RX: %u bytes\n", mock->total_rx_bytes);
    printf("  - Total transfers: %u\n", mock->total_transfers);
#endif
    
    mock->initialized = false;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Mock SPI Send
 * Simulates sending data (stores in TX buffer and echoes to RX)
 */
static alp_status_t mock_send(void *hw, const void *data, uint32_t len)
{
    if (!hw || !data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (mock->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    // Simulate busy state
    mock->busy = true;
    
    // Copy to TX buffer (simulate hardware FIFO)
    uint32_t to_copy = len < MOCK_SPI_BUFFER_SIZE ? len : MOCK_SPI_BUFFER_SIZE;
    memcpy(mock->tx_buffer, data, to_copy);
    mock->tx_count = to_copy;
    
    // Simulate loopback: TX data echoes to RX buffer
    // This simulates a device echoing back data or MISO receiving while MOSI sends
    memcpy(mock->rx_buffer, data, to_copy);
    mock->rx_count = to_copy;
    
    // Update statistics
    mock->total_tx_bytes += len;
    mock->total_transfers++;
    
#if MOCK_SPI_VERBOSE
    printf("[MOCK SPI%d] 📤 Send %u bytes: ", mock->config.instance, len);
    const uint8_t *bytes = (const uint8_t*)data;
    for (uint32_t i = 0; i < len && i < 16; i++) {
        printf("%02X ", bytes[i]);
    }
    if (len > 16) printf("...");
    printf("\n");
#endif
    
    // Simulate transfer complete (instant for mock)
    mock->busy = false;
    
    // Call event callback if registered
    if (mock->event_cb) {
        // Simulate ARM_SPI_EVENT_TRANSFER_COMPLETE
        mock->event_cb(0x01);
    }
    
    return ALP_STATUS_OK;
}

/**
 * @brief Mock SPI Receive
 * Returns previously received data (from loopback or simulated data)
 */
static alp_status_t mock_receive(void *hw, void *data, uint32_t len)
{
    if (!hw || !data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (mock->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    mock->busy = true;
    
    // Copy from RX buffer (simulate receiving from hardware FIFO)
    uint32_t to_copy = len < mock->rx_count ? len : mock->rx_count;
    
    if (to_copy == 0) {
        // No data available - simulate receiving zeros
        memset(data, 0, len);
        to_copy = len;
    } else {
        memcpy(data, mock->rx_buffer, to_copy);
    }
    
    // Update statistics
    mock->total_rx_bytes += len;
    
#if MOCK_SPI_VERBOSE
    printf("[MOCK SPI%d] 📥 Receive %u bytes: ", mock->config.instance, len);
    const uint8_t *bytes = (const uint8_t*)data;
    for (uint32_t i = 0; i < len && i < 16; i++) {
        printf("%02X ", bytes[i]);
    }
    if (len > 16) printf("...");
    printf("\n");
#endif
    
    mock->busy = false;
    
    if (mock->event_cb) {
        mock->event_cb(0x01); // Transfer complete
    }
    
    return ALP_STATUS_OK;
}

/**
 * @brief Mock SPI Transfer (Full-duplex)
 * Simulates simultaneous send and receive
 */
static alp_status_t mock_transfer(void *hw, const void *tx_data, void *rx_data, uint32_t len)
{
    if (!hw || !tx_data || !rx_data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (mock->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    mock->busy = true;
    
    // Simulate full-duplex: send TX and receive RX
    // In real hardware, MISO and MOSI happen simultaneously
    // For mock, we echo TX data to RX (loopback simulation)
    memcpy(rx_data, tx_data, len);
    
    // Update statistics
    mock->total_tx_bytes += len;
    mock->total_rx_bytes += len;
    mock->total_transfers++;
    
#if MOCK_SPI_VERBOSE
    printf("[MOCK SPI%d] 🔄 Transfer %u bytes\n", mock->config.instance, len);
    printf("  TX: ");
    const uint8_t *tx_bytes = (const uint8_t*)tx_data;
    for (uint32_t i = 0; i < len && i < 8; i++) {
        printf("%02X ", tx_bytes[i]);
    }
    if (len > 8) printf("...");
    printf("\n");
    
    printf("  RX: ");
    const uint8_t *rx_bytes = (const uint8_t*)rx_data;
    for (uint32_t i = 0; i < len && i < 8; i++) {
        printf("%02X ", rx_bytes[i]);
    }
    if (len > 8) printf("...");
    printf("\n");
#endif
    
    mock->busy = false;
    
    if (mock->event_cb) {
        mock->event_cb(0x01); // Transfer complete
    }
    
    return ALP_STATUS_OK;
}

/**
 * @brief Mock SPI Control
 * Simulates control commands (e.g., set baudrate)
 */
static alp_status_t mock_control(void *hw, uint32_t cmd, uint32_t arg)
{
    if (!hw) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
#if MOCK_SPI_VERBOSE
    printf("[MOCK SPI%d] ⚙️  Control: cmd=0x%08X, arg=%u\n", 
           mock->config.instance, cmd, arg);
#endif
    
    // Mock accepts all control commands (no-op)
    return ALP_STATUS_OK;
}

/**
 * @brief Get Mock SPI Status
 */
static alp_status_t mock_get_status(void *hw, alp_spi_status_t *status)
{
    if (!hw || !status) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    
    if (!mock->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    // Fill status
    memset(status, 0, sizeof(alp_spi_status_t));
    status->busy = mock->busy ? 1 : 0;
    status->data_lost = mock->data_lost ? 1 : 0;
    status->mode_fault = 0;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Check if Mock SPI is busy
 */
static bool mock_is_busy(void *hw)
{
    if (!hw) {
        return false;
    }
    
    mock_spi_hw_t *mock = (mock_spi_hw_t*)hw;
    return mock->busy;
}

/************************************
 * GLOBAL FUNCTIONS
 ************************************/

/**
 * @brief Mock SPI Virtual Function Table
 * This is the VFT that will be assigned to mock handles
 */
static const alp_spi_ops_t mock_spi_ops = {
    .init = mock_init,
    .deinit = mock_deinit,
    .send = mock_send,
    .receive = mock_receive,
    .transfer = mock_transfer,
    .control = mock_control,
    .get_status = mock_get_status,
    .is_busy = mock_is_busy
};

/**
 * @brief Factory function: Create Mock SPI handle
 * @param instance SPI instance number (0-3)
 * @return Pointer to SPI handle, or NULL on error
 * 
 * @note This is the MAGIC function - it creates a working SPI driver
 *       WITHOUT any vendor SDK!
 */
alp_spi_handle_t* alp_spi_create_mock(uint32_t instance)
{
    if (instance >= ALP_SPI_MAX_INSTANCES) {
        printf("[MOCK SPI] ❌ Invalid instance: %u\n", instance);
        return NULL;
    }
    
    // Allocate handle
    alp_spi_handle_t *handle = (alp_spi_handle_t*)malloc(sizeof(alp_spi_handle_t));
    if (!handle) {
        printf("[MOCK SPI] ❌ Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_spi_handle_t));
    
    // Assign VFT (this is where the magic happens!)
    handle->ops = &mock_spi_ops;
    
    // Assign hardware handle (point to global mock hardware)
    handle->hw_handle = &g_mock_spi[instance];
    
    // Set platform name
    handle->platform_name = "mock";
    
    // Set instance in config
    handle->config.instance = instance;
    
    printf("\n[MOCK SPI%d] 🎉 Created (NO VENDOR SDK REQUIRED!)\n", instance);
    printf("  - Platform: %s\n", handle->platform_name);
    printf("  - VFT: %p\n", (void*)handle->ops);
    printf("  - Hardware: Simulated\n\n");
    
    return handle;
}
