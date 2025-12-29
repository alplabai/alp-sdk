/**
 ********************************************************************************
 * @file    alp_usart_vft.h
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   ALP USART Driver - Virtual Function Table Interface
 * @note    Platform-independent USART interface using VFT pattern
 ********************************************************************************
 */

#ifndef ALP_USART_VFT_H
#define ALP_USART_VFT_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/************************************
 * PUBLIC TYPES
 ************************************/

/**
 * @brief USART status codes
 */
typedef enum {
    ALP_USART_OK = 0,
    ALP_USART_ERROR = -1,
    ALP_USART_ERROR_PARAMETER = -2,
    ALP_USART_ERROR_NOT_INITIALIZED = -3,
    ALP_USART_ERROR_BUSY = -4,
    ALP_USART_ERROR_TIMEOUT = -5
} alp_usart_status_t;

/**
 * @brief USART data bits
 */
typedef enum {
    ALP_USART_DATA_BITS_5 = 5,
    ALP_USART_DATA_BITS_6 = 6,
    ALP_USART_DATA_BITS_7 = 7,
    ALP_USART_DATA_BITS_8 = 8,
    ALP_USART_DATA_BITS_9 = 9
} alp_usart_data_bits_t;

/**
 * @brief USART parity
 */
typedef enum {
    ALP_USART_PARITY_NONE = 0,
    ALP_USART_PARITY_EVEN = 1,
    ALP_USART_PARITY_ODD = 2
} alp_usart_parity_t;

/**
 * @brief USART stop bits
 */
typedef enum {
    ALP_USART_STOP_BITS_1 = 1,
    ALP_USART_STOP_BITS_2 = 2
} alp_usart_stop_bits_t;

/**
 * @brief USART flow control
 */
typedef enum {
    ALP_USART_FLOW_CONTROL_NONE = 0,
    ALP_USART_FLOW_CONTROL_RTS = 1,
    ALP_USART_FLOW_CONTROL_CTS = 2,
    ALP_USART_FLOW_CONTROL_RTS_CTS = 3
} alp_usart_flow_control_t;

/**
 * @brief USART event types
 */
typedef enum {
    ALP_USART_EVENT_TX_COMPLETE = 0,
    ALP_USART_EVENT_RX_COMPLETE = 1,
    ALP_USART_EVENT_RX_TIMEOUT = 2,
    ALP_USART_EVENT_ERROR = 3
} alp_usart_event_t;

/**
 * @brief USART configuration structure
 */
typedef struct {
    uint32_t baudrate;
    alp_usart_data_bits_t data_bits;
    alp_usart_parity_t parity;
    alp_usart_stop_bits_t stop_bits;
    alp_usart_flow_control_t flow_control;
    uint8_t instance;
} alp_usart_config_t;

/**
 * @brief USART event callback
 */
typedef void (*alp_usart_event_cb_t)(alp_usart_event_t event);

/************************************
 * FORWARD DECLARATIONS
 ************************************/
typedef struct alp_usart_handle_t alp_usart_handle_t;

/**
 * @brief USART operations (VFT)
 */
typedef struct {
    alp_usart_status_t (*init)(void *hw, const alp_usart_config_t *cfg, alp_usart_event_cb_t event_cb);
    alp_usart_status_t (*deinit)(void *hw);
    alp_usart_status_t (*send)(void *hw, const uint8_t *data, size_t size);
    alp_usart_status_t (*receive)(void *hw, uint8_t *data, size_t size, uint32_t timeout_ms);
    alp_usart_status_t (*get_tx_count)(void *hw, size_t *count);
    alp_usart_status_t (*get_rx_count)(void *hw, size_t *count);
} alp_usart_ops_t;

/**
 * @brief USART handle
 */
struct alp_usart_handle_t {
    const alp_usart_ops_t *ops;
    void *hw_handle;
    const char *platform_name;
    alp_usart_config_t config;
};

/************************************
 * PUBLIC FUNCTION PROTOTYPES
 ************************************/

/**
 * @brief Initialize USART
 * @param handle USART handle
 * @param cfg Configuration structure
 * @param event_cb Event callback (optional)
 * @return Status code
 */
alp_usart_status_t alp_usart_init(alp_usart_handle_t *handle, 
                                   const alp_usart_config_t *cfg,
                                   alp_usart_event_cb_t event_cb);

/**
 * @brief Deinitialize USART
 * @param handle USART handle
 * @return Status code
 */
alp_usart_status_t alp_usart_deinit(alp_usart_handle_t *handle);

/**
 * @brief Send data over USART
 * @param handle USART handle
 * @param data Data buffer
 * @param size Number of bytes to send
 * @return Status code
 */
alp_usart_status_t alp_usart_send(alp_usart_handle_t *handle, 
                                   const uint8_t *data, 
                                   size_t size);

/**
 * @brief Receive data from USART
 * @param handle USART handle
 * @param data Data buffer
 * @param size Number of bytes to receive
 * @param timeout_ms Timeout in milliseconds
 * @return Status code
 */
alp_usart_status_t alp_usart_receive(alp_usart_handle_t *handle, 
                                      uint8_t *data, 
                                      size_t size,
                                      uint32_t timeout_ms);

/**
 * @brief Get transmitted byte count
 * @param handle USART handle
 * @param count Pointer to store count
 * @return Status code
 */
alp_usart_status_t alp_usart_get_tx_count(alp_usart_handle_t *handle, size_t *count);

/**
 * @brief Get received byte count
 * @param handle USART handle
 * @param count Pointer to store count
 * @return Status code
 */
alp_usart_status_t alp_usart_get_rx_count(alp_usart_handle_t *handle, size_t *count);

/**
 * @brief Print formatted string (printf-like)
 * @param handle USART handle
 * @param format Format string
 * @return Number of characters sent
 */
int alp_usart_printf(alp_usart_handle_t *handle, const char *format, ...);

/************************************
 * PLATFORM-SPECIFIC FACTORY FUNCTIONS
 ************************************/

/**
 * @brief Create Mock USART instance
 * @param instance USART instance number
 * @return USART handle or NULL on error
 */
alp_usart_handle_t* alp_usart_create_mock(uint8_t instance);

/**
 * @brief Create Alif USART instance
 * @param instance USART instance number
 * @return USART handle or NULL on error
 */
alp_usart_handle_t* alp_usart_create_alif(uint8_t instance);

#ifdef __cplusplus
}
#endif

#endif /* ALP_USART_VFT_H */
