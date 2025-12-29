/**
 ********************************************************************************
 * @file    alp_spi_alif.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Alif Ensemble SPI Driver - VFT Implementation
 * @note    This driver requires Alif Ensemble SDK (CMSIS-Driver)
 *          Based on existing alp_spi.c implementation
 ********************************************************************************
 */

/************************************
 * INCLUDES
 ************************************/
#include "alp_spi_vft.h"
#include "Driver_SPI.h"  // Alif CMSIS-Driver
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
#define ALIF_SPI_MAX_INSTANCES  2   // SPI0, SPI1 (Alif E7)
#define ALIF_SPI_VERBOSE        1   // Enable detailed logging

/************************************
 * PRIVATE TYPEDEFS
 ************************************/

/**
 * @brief Alif SPI Hardware State
 * Wraps the CMSIS ARM_DRIVER_SPI interface
 */
typedef struct {
    // CMSIS Driver
    ARM_DRIVER_SPI *cmsis_driver;
    uint32_t instance;
    
    // Configuration
    alp_spi_config_t config;
    alp_spi_event_cb_t event_cb;
    
    // Status
    bool initialized;
    bool powered;
    volatile bool busy;
    volatile bool transfer_complete;
    
    // Statistics
    uint32_t total_tx_bytes;
    uint32_t total_rx_bytes;
    uint32_t total_transfers;
    
} alif_spi_hw_t;

/************************************
 * STATIC VARIABLES
 ************************************/

// External CMSIS drivers (from Alif SDK)
extern ARM_DRIVER_SPI Driver_SPI0;
extern ARM_DRIVER_SPI Driver_SPI1;

// Global Alif hardware instances
static alif_spi_hw_t g_alif_spi[ALIF_SPI_MAX_INSTANCES] = {0};

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
static alp_status_t alif_init(void *hw, const alp_spi_config_t *cfg, alp_spi_event_cb_t event_cb);
static alp_status_t alif_deinit(void *hw);
static alp_status_t alif_send(void *hw, const void *data, uint32_t len);
static alp_status_t alif_receive(void *hw, void *data, uint32_t len);
static alp_status_t alif_transfer(void *hw, const void *tx_data, void *rx_data, uint32_t len);
static alp_status_t alif_control(void *hw, uint32_t cmd, uint32_t arg);
static alp_status_t alif_get_status(void *hw, alp_spi_status_t *status);
static bool alif_is_busy(void *hw);

// CMSIS callback
static void alif_spi_callback(uint32_t event);

/************************************
 * STATIC FUNCTIONS
 ************************************/

/**
 * @brief Get CMSIS driver for instance
 */
static ARM_DRIVER_SPI* get_cmsis_driver(uint32_t instance)
{
    switch (instance) {
        case 0: return &Driver_SPI0;
        case 1: return &Driver_SPI1;
        default: return NULL;
    }
}

/**
 * @brief Convert ALP status to CMSIS status
 */
static int32_t convert_alp_to_cmsis_mode(alp_spi_mode_t mode, 
                                         alp_spi_clock_mode_t clock,
                                         alp_spi_bit_order_t order,
                                         alp_spi_ss_mode_t ss_mode,
                                         uint8_t data_bits)
{
    int32_t control = 0;
    
    // Mode
    switch (mode) {
        case ALP_SPI_MODE_MASTER:
            control |= ARM_SPI_MODE_MASTER;
            break;
        case ALP_SPI_MODE_SLAVE:
            control |= ARM_SPI_MODE_SLAVE;
            break;
        case ALP_SPI_MODE_MASTER_SIMPLEX:
            control |= ARM_SPI_MODE_MASTER_SIMPLEX;
            break;
        case ALP_SPI_MODE_SLAVE_SIMPLEX:
            control |= ARM_SPI_MODE_SLAVE_SIMPLEX;
            break;
    }
    
    // Clock polarity and phase
    switch (clock) {
        case ALP_SPI_CPOL0_CPHA0:
            control |= ARM_SPI_CPOL0_CPHA0;
            break;
        case ALP_SPI_CPOL0_CPHA1:
            control |= ARM_SPI_CPOL0_CPHA1;
            break;
        case ALP_SPI_CPOL1_CPHA0:
            control |= ARM_SPI_CPOL1_CPHA0;
            break;
        case ALP_SPI_CPOL1_CPHA1:
            control |= ARM_SPI_CPOL1_CPHA1;
            break;
    }
    
    // Bit order
    if (order == ALP_SPI_LSB_FIRST) {
        control |= ARM_SPI_LSB_MSB;
    } else {
        control |= ARM_SPI_MSB_LSB;
    }
    
    // Slave select mode
    switch (ss_mode) {
        case ALP_SPI_SS_MASTER_UNUSED:
            control |= ARM_SPI_SS_MASTER_UNUSED;
            break;
        case ALP_SPI_SS_MASTER_SW:
            control |= ARM_SPI_SS_MASTER_SW;
            break;
        case ALP_SPI_SS_MASTER_HW_OUTPUT:
            control |= ARM_SPI_SS_MASTER_HW_OUTPUT;
            break;
        case ALP_SPI_SS_SLAVE_HW:
            control |= ARM_SPI_SS_SLAVE_HW;
            break;
        case ALP_SPI_SS_SLAVE_SW:
            control |= ARM_SPI_SS_SLAVE_SW;
            break;
    }
    
    // Data bits
    control |= ARM_SPI_DATA_BITS(data_bits);
    
    return control;
}

/**
 * @brief CMSIS SPI Callback
 * Called by CMSIS driver when transfer completes
 */
static void alif_spi_callback(uint32_t event)
{
    // Find which instance triggered the event
    // For now, we'll handle this in the instance-specific callbacks
    // This is a simplified version - in production, you'd need instance tracking
    
    if (event & ARM_SPI_EVENT_TRANSFER_COMPLETE) {
        // Mark transfer as complete
        // Instance-specific handling would go here
    }
    
    if (event & ARM_SPI_EVENT_DATA_LOST) {
        // Handle data lost
    }
    
    if (event & ARM_SPI_EVENT_MODE_FAULT) {
        // Handle mode fault
    }
}

/**
 * @brief Initialize Alif SPI hardware
 */
static alp_status_t alif_init(void *hw, const alp_spi_config_t *cfg, alp_spi_event_cb_t event_cb)
{
    if (!hw || !cfg) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    // Get CMSIS driver
    alif->cmsis_driver = get_cmsis_driver(cfg->instance);
    if (!alif->cmsis_driver) {
        printf("[ALIF SPI%d] ❌ Invalid instance\n", cfg->instance);
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    // Store configuration
    memcpy(&alif->config, cfg, sizeof(alp_spi_config_t));
    alif->event_cb = event_cb;
    alif->instance = cfg->instance;
    
#if ALIF_SPI_VERBOSE
    printf("\n[ALIF SPI%d] 🔧 Initializing...\n", cfg->instance);
#endif
    
    // Initialize CMSIS driver
    int32_t ret = alif->cmsis_driver->Initialize(alif_spi_callback);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF SPI%d] ❌ Initialize failed: %d\n", cfg->instance, ret);
        return ALP_STATUS_ERROR;
    }
    
    // Power on
    ret = alif->cmsis_driver->PowerControl(ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF SPI%d] ❌ PowerControl failed: %d\n", cfg->instance, ret);
        alif->cmsis_driver->Uninitialize();
        return ALP_STATUS_ERROR;
    }
    alif->powered = true;
    
    // Configure SPI
    int32_t control = convert_alp_to_cmsis_mode(
        cfg->mode,
        cfg->clock_mode,
        cfg->bit_order,
        cfg->ss_mode,
        cfg->data_bits
    );
    
    ret = alif->cmsis_driver->Control(control, cfg->baudrate);
    if (ret != ARM_DRIVER_OK) {
        printf("[ALIF SPI%d] ❌ Control failed: %d\n", cfg->instance, ret);
        alif->cmsis_driver->PowerControl(ARM_POWER_OFF);
        alif->cmsis_driver->Uninitialize();
        return ALP_STATUS_ERROR;
    }
    
    alif->initialized = true;
    
#if ALIF_SPI_VERBOSE
    printf("[ALIF SPI%d] ✅ Initialized\n", cfg->instance);
    printf("  - Mode: %s\n", 
           cfg->mode == ALP_SPI_MODE_MASTER ? "Master" : "Slave");
    printf("  - Baudrate: %u Hz\n", cfg->baudrate);
    printf("  - Data bits: %u\n", cfg->data_bits);
    printf("  - Clock mode: CPOL%d_CPHA%d\n", 
           (cfg->clock_mode >> 1) & 1, cfg->clock_mode & 1);
#endif
    
    return ALP_STATUS_OK;
}

/**
 * @brief Deinitialize Alif SPI hardware
 */
static alp_status_t alif_deinit(void *hw)
{
    if (!hw) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
#if ALIF_SPI_VERBOSE
    printf("\n[ALIF SPI%d] 🔌 Deinitializing\n", alif->instance);
    printf("  - Total TX: %u bytes\n", alif->total_tx_bytes);
    printf("  - Total RX: %u bytes\n", alif->total_rx_bytes);
    printf("  - Total transfers: %u\n", alif->total_transfers);
#endif
    
    // Power off
    if (alif->powered) {
        alif->cmsis_driver->PowerControl(ARM_POWER_OFF);
        alif->powered = false;
    }
    
    // Uninitialize
    alif->cmsis_driver->Uninitialize();
    alif->initialized = false;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Alif SPI Send
 */
static alp_status_t alif_send(void *hw, const void *data, uint32_t len)
{
    if (!hw || !data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (alif->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    alif->busy = true;
    alif->transfer_complete = false;
    
#if ALIF_SPI_VERBOSE
    printf("[ALIF SPI%d] 📤 Send %u bytes\n", alif->instance, len);
#endif
    
    // Send via CMSIS driver
    int32_t ret = alif->cmsis_driver->Send(data, len);
    
    if (ret != ARM_DRIVER_OK) {
        alif->busy = false;
        printf("[ALIF SPI%d] ❌ Send failed: %d\n", alif->instance, ret);
        return ALP_STATUS_ERROR;
    }
    
    // Wait for transfer to complete (blocking mode)
    // In production, you might want async/callback mode
    while (!alif->transfer_complete && alif->busy) {
        ARM_SPI_STATUS status = alif->cmsis_driver->GetStatus();
        if (!status.busy) {
            alif->transfer_complete = true;
            break;
        }
    }
    
    alif->busy = false;
    alif->total_tx_bytes += len;
    alif->total_transfers++;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Alif SPI Receive
 */
static alp_status_t alif_receive(void *hw, void *data, uint32_t len)
{
    if (!hw || !data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (alif->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    alif->busy = true;
    alif->transfer_complete = false;
    
#if ALIF_SPI_VERBOSE
    printf("[ALIF SPI%d] 📥 Receive %u bytes\n", alif->instance, len);
#endif
    
    // Receive via CMSIS driver
    int32_t ret = alif->cmsis_driver->Receive(data, len);
    
    if (ret != ARM_DRIVER_OK) {
        alif->busy = false;
        printf("[ALIF SPI%d] ❌ Receive failed: %d\n", alif->instance, ret);
        return ALP_STATUS_ERROR;
    }
    
    // Wait for transfer to complete
    while (!alif->transfer_complete && alif->busy) {
        ARM_SPI_STATUS status = alif->cmsis_driver->GetStatus();
        if (!status.busy) {
            alif->transfer_complete = true;
            break;
        }
    }
    
    alif->busy = false;
    alif->total_rx_bytes += len;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Alif SPI Transfer (Full-duplex)
 */
static alp_status_t alif_transfer(void *hw, const void *tx_data, void *rx_data, uint32_t len)
{
    if (!hw || !tx_data || !rx_data || len == 0) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    if (alif->busy) {
        return ALP_STATUS_ERROR_BUSY;
    }
    
    alif->busy = true;
    alif->transfer_complete = false;
    
#if ALIF_SPI_VERBOSE
    printf("[ALIF SPI%d] 🔄 Transfer %u bytes\n", alif->instance, len);
#endif
    
    // Transfer via CMSIS driver
    int32_t ret = alif->cmsis_driver->Transfer(tx_data, rx_data, len);
    
    if (ret != ARM_DRIVER_OK) {
        alif->busy = false;
        printf("[ALIF SPI%d] ❌ Transfer failed: %d\n", alif->instance, ret);
        return ALP_STATUS_ERROR;
    }
    
    // Wait for transfer to complete
    while (!alif->transfer_complete && alif->busy) {
        ARM_SPI_STATUS status = alif->cmsis_driver->GetStatus();
        if (!status.busy) {
            alif->transfer_complete = true;
            break;
        }
    }
    
    alif->busy = false;
    alif->total_tx_bytes += len;
    alif->total_rx_bytes += len;
    alif->total_transfers++;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Alif SPI Control
 */
static alp_status_t alif_control(void *hw, uint32_t cmd, uint32_t arg)
{
    if (!hw) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
#if ALIF_SPI_VERBOSE
    printf("[ALIF SPI%d] ⚙️  Control: cmd=0x%08X, arg=%u\n", 
           alif->instance, cmd, arg);
#endif
    
    // Pass through to CMSIS driver
    int32_t ret = alif->cmsis_driver->Control(cmd, arg);
    
    if (ret != ARM_DRIVER_OK) {
        return ALP_STATUS_ERROR;
    }
    
    return ALP_STATUS_OK;
}

/**
 * @brief Get Alif SPI Status
 */
static alp_status_t alif_get_status(void *hw, alp_spi_status_t *status)
{
    if (!hw || !status) {
        return ALP_STATUS_ERROR_PARAMETER;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return ALP_STATUS_ERROR_NOT_INITIALIZED;
    }
    
    // Get CMSIS status
    ARM_SPI_STATUS cmsis_status = alif->cmsis_driver->GetStatus();
    
    // Convert to ALP status
    memset(status, 0, sizeof(alp_spi_status_t));
    status->busy = cmsis_status.busy;
    status->data_lost = cmsis_status.data_lost;
    status->mode_fault = cmsis_status.mode_fault;
    
    return ALP_STATUS_OK;
}

/**
 * @brief Check if Alif SPI is busy
 */
static bool alif_is_busy(void *hw)
{
    if (!hw) {
        return false;
    }
    
    alif_spi_hw_t *alif = (alif_spi_hw_t*)hw;
    
    if (!alif->initialized) {
        return false;
    }
    
    ARM_SPI_STATUS status = alif->cmsis_driver->GetStatus();
    return status.busy;
}

/************************************
 * GLOBAL FUNCTIONS
 ************************************/

/**
 * @brief Alif SPI Virtual Function Table
 */
static const alp_spi_ops_t alif_spi_ops = {
    .init = alif_init,
    .deinit = alif_deinit,
    .send = alif_send,
    .receive = alif_receive,
    .transfer = alif_transfer,
    .control = alif_control,
    .get_status = alif_get_status,
    .is_busy = alif_is_busy
};

/**
 * @brief Factory function: Create Alif SPI handle
 * @param instance SPI instance number (0-1)
 * @return Pointer to SPI handle, or NULL on error
 * 
 * @note This requires Alif Ensemble SDK to be available
 */
alp_spi_handle_t* alp_spi_create_alif(uint32_t instance)
{
    if (instance >= ALIF_SPI_MAX_INSTANCES) {
        printf("[ALIF SPI] ❌ Invalid instance: %u\n", instance);
        return NULL;
    }
    
    // Allocate handle
    alp_spi_handle_t *handle = (alp_spi_handle_t*)malloc(sizeof(alp_spi_handle_t));
    if (!handle) {
        printf("[ALIF SPI] ❌ Failed to allocate handle\n");
        return NULL;
    }
    
    // Clear handle
    memset(handle, 0, sizeof(alp_spi_handle_t));
    
    // Assign VFT (Alif implementation)
    handle->ops = &alif_spi_ops;
    
    // Assign hardware handle (point to global Alif hardware)
    handle->hw_handle = &g_alif_spi[instance];
    
    // Set platform name
    handle->platform_name = "alif";
    
    // Set instance in config
    handle->config.instance = instance;
    
    printf("\n[ALIF SPI%d] 🎉 Created (Alif Ensemble CMSIS Driver)\n", instance);
    printf("  - Platform: %s\n", handle->platform_name);
    printf("  - VFT: %p\n", (void*)handle->ops);
    printf("  - Hardware: Alif Ensemble E7\n\n");
    
    return handle;
}
