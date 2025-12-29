/**
 ********************************************************************************
 * @file    alp_spi_vft.h
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   SPI Virtual Function Table (VFT) Interface - Platform independent
 * @note    This file implements Mimari 1: VFT Pattern + Mock Driver
 ********************************************************************************
 */

#ifndef ALP_SPI_VFT_H
#define ALP_SPI_VFT_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/************************************
 * MACROS AND DEFINES
 ************************************/
#define ALP_SPI_MAX_INSTANCES   4

/************************************
 * TYPEDEFS
 ************************************/

/**
 * @brief ALP Status codes
 */
typedef enum {
    ALP_STATUS_OK = 0,
    ALP_STATUS_ERROR = -1,
    ALP_STATUS_ERROR_BUSY = -2,
    ALP_STATUS_ERROR_TIMEOUT = -3,
    ALP_STATUS_ERROR_UNSUPPORTED = -4,
    ALP_STATUS_ERROR_PARAMETER = -5,
    ALP_STATUS_ERROR_NOT_INITIALIZED = -6
} alp_status_t;

/**
 * @brief SPI Mode
 */
typedef enum {
    ALP_SPI_MODE_MASTER = 0,
    ALP_SPI_MODE_SLAVE = 1,
    ALP_SPI_MODE_MASTER_SIMPLEX = 2,
    ALP_SPI_MODE_SLAVE_SIMPLEX = 3
} alp_spi_mode_t;

/**
 * @brief SPI Clock Polarity and Phase
 */
typedef enum {
    ALP_SPI_CPOL0_CPHA0 = 0,  // Clock idle low, data captured on rising edge
    ALP_SPI_CPOL0_CPHA1 = 1,  // Clock idle low, data captured on falling edge
    ALP_SPI_CPOL1_CPHA0 = 2,  // Clock idle high, data captured on falling edge
    ALP_SPI_CPOL1_CPHA1 = 3   // Clock idle high, data captured on rising edge
} alp_spi_clock_mode_t;

/**
 * @brief SPI Bit Order
 */
typedef enum {
    ALP_SPI_MSB_FIRST = 0,
    ALP_SPI_LSB_FIRST = 1
} alp_spi_bit_order_t;

/**
 * @brief SPI Slave Select Mode
 */
typedef enum {
    ALP_SPI_SS_MASTER_UNUSED = 0,
    ALP_SPI_SS_MASTER_SW = 1,
    ALP_SPI_SS_MASTER_HW_OUTPUT = 2,
    ALP_SPI_SS_SLAVE_HW = 3,
    ALP_SPI_SS_SLAVE_SW = 4
} alp_spi_ss_mode_t;

/**
 * @brief SPI Status
 */
typedef struct {
    uint32_t busy : 1;              // Transmitter/Receiver busy flag
    uint32_t data_lost : 1;         // Data lost: Receive overflow / Transmit underflow
    uint32_t mode_fault : 1;        // Mode fault detected
    uint32_t reserved : 29;
} alp_spi_status_t;

/**
 * @brief SPI Configuration
 */
typedef struct {
    uint32_t instance;              // SPI instance number (0, 1, 2, 3)
    uint32_t baudrate;              // Baudrate in Hz
    uint8_t data_bits;              // Data bits (4-16, typically 8 or 16)
    alp_spi_mode_t mode;            // Master/Slave mode
    alp_spi_clock_mode_t clock_mode; // Clock polarity and phase
    alp_spi_bit_order_t bit_order;  // MSB/LSB first
    alp_spi_ss_mode_t ss_mode;      // Slave select mode
} alp_spi_config_t;

/**
 * @brief SPI Event callback type
 * @param event Event flags
 */
typedef void (*alp_spi_event_cb_t)(uint32_t event);

// Forward declaration
struct alp_spi_handle_s;

/**
 * @brief SPI Virtual Function Table (VFT)
 * This is the core of the VFT pattern - each platform implements these functions
 */
typedef struct alp_spi_ops {
    /**
     * @brief Initialize SPI hardware
     * @param hw Platform-specific hardware handle (opaque pointer)
     * @param cfg Configuration structure
     * @param event_cb Event callback function
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*init)(void *hw, const alp_spi_config_t *cfg, alp_spi_event_cb_t event_cb);
    
    /**
     * @brief Deinitialize SPI hardware
     * @param hw Platform-specific hardware handle
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*deinit)(void *hw);
    
    /**
     * @brief Send data via SPI
     * @param hw Platform-specific hardware handle
     * @param data Pointer to data buffer
     * @param len Number of data items to send
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*send)(void *hw, const void *data, uint32_t len);
    
    /**
     * @brief Receive data via SPI
     * @param hw Platform-specific hardware handle
     * @param data Pointer to receive buffer
     * @param len Number of data items to receive
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*receive)(void *hw, void *data, uint32_t len);
    
    /**
     * @brief Full-duplex transfer (simultaneous send and receive)
     * @param hw Platform-specific hardware handle
     * @param tx_data Pointer to transmit buffer
     * @param rx_data Pointer to receive buffer
     * @param len Number of data items to transfer
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*transfer)(void *hw, const void *tx_data, void *rx_data, uint32_t len);
    
    /**
     * @brief Control SPI (set baudrate, etc.)
     * @param hw Platform-specific hardware handle
     * @param cmd Control command
     * @param arg Command argument
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*control)(void *hw, uint32_t cmd, uint32_t arg);
    
    /**
     * @brief Get current SPI status
     * @param hw Platform-specific hardware handle
     * @param status Pointer to status structure
     * @return ALP_STATUS_OK on success
     */
    alp_status_t (*get_status)(void *hw, alp_spi_status_t *status);
    
    /**
     * @brief Check if SPI is busy
     * @param hw Platform-specific hardware handle
     * @return true if busy, false otherwise
     */
    bool (*is_busy)(void *hw);
    
} alp_spi_ops_t;

/**
 * @brief SPI Handle
 * This is the main handle that users interact with.
 * It contains the VFT and platform-specific data.
 */
typedef struct alp_spi_handle_s {
    const alp_spi_ops_t *ops;       // Virtual function table (platform-specific)
    void *hw_handle;                // Platform-specific hardware handle (opaque)
    alp_spi_config_t config;        // Current configuration
    const char *platform_name;      // "mock", "alif", "renesas", etc.
    bool initialized;               // Initialization flag
} alp_spi_handle_t;

/************************************
 * EXPORTED VARIABLES
 ************************************/
// None

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/

/**
 * @brief Create a Mock SPI handle (NO VENDOR SDK REQUIRED!)
 * @param instance SPI instance number (0-3)
 * @return Pointer to SPI handle, or NULL on error
 */
alp_spi_handle_t* alp_spi_create_mock(uint32_t instance);

/**
 * @brief Create an Alif SPI handle (requires Alif SDK)
 * @param instance SPI instance number (0-3)
 * @return Pointer to SPI handle, or NULL on error
 */
alp_spi_handle_t* alp_spi_create_alif(uint32_t instance);

/**
 * @brief Destroy SPI handle and free resources
 * @param handle Pointer to SPI handle
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_destroy(alp_spi_handle_t *handle);

/**
 * @brief Initialize SPI with given configuration
 * @param handle SPI handle
 * @param config Configuration structure
 * @param event_cb Event callback function (can be NULL)
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_init(alp_spi_handle_t *handle, const alp_spi_config_t *config, alp_spi_event_cb_t event_cb);

/**
 * @brief Deinitialize SPI
 * @param handle SPI handle
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_deinit(alp_spi_handle_t *handle);

/**
 * @brief Send data via SPI
 * @param handle SPI handle
 * @param data Pointer to data buffer
 * @param len Number of data items to send
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_send(alp_spi_handle_t *handle, const void *data, uint32_t len);

/**
 * @brief Receive data via SPI
 * @param handle SPI handle
 * @param data Pointer to receive buffer
 * @param len Number of data items to receive
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_receive(alp_spi_handle_t *handle, void *data, uint32_t len);

/**
 * @brief Full-duplex transfer
 * @param handle SPI handle
 * @param tx_data Pointer to transmit buffer
 * @param rx_data Pointer to receive buffer
 * @param len Number of data items to transfer
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_transfer(alp_spi_handle_t *handle, const void *tx_data, void *rx_data, uint32_t len);

/**
 * @brief Control SPI
 * @param handle SPI handle
 * @param cmd Control command
 * @param arg Command argument
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_control(alp_spi_handle_t *handle, uint32_t cmd, uint32_t arg);

/**
 * @brief Get SPI status
 * @param handle SPI handle
 * @param status Pointer to status structure
 * @return ALP_STATUS_OK on success
 */
alp_status_t alp_spi_get_status(alp_spi_handle_t *handle, alp_spi_status_t *status);

/**
 * @brief Check if SPI is busy
 * @param handle SPI handle
 * @return true if busy, false otherwise
 */
bool alp_spi_is_busy(alp_spi_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* ALP_SPI_VFT_H */
