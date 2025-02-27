/**
 ********************************************************************************
 * @file    alp_spi.h
 * @author  Sukru Aydogdu
 * @date    16/1/2025
 * @brief   Header file for ALP SPI  functions
 ********************************************************************************
 */

#ifndef ALP_SPI_H
#define ALP_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include "stdint.h"
#include "alp_driver_common.h"
#include "Driver_SPI.h"

/************************************
 * MACROS AND DEFINES
 ************************************/
/****** SPI Control Codes *****/

#define ALP_SPI_CONTROL_Pos ARM_SPI_CONTROL_Pos
#define ALP_SPI_CONTROL_Msk ARM_SPI_CONTROL_Msk

/*----- SPI Control Codes: Mode -----*/
#define ALP_SPI_MODE_INACTIVE ARM_SPI_MODE_INACTIVE
#define ALP_SPI_MODE_MASTER ARM_SPI_MODE_MASTER
#define ALP_SPI_MODE_SLAVE ARM_SPI_MODE_SLAVE
#define ALP_SPI_MODE_MASTER_SIMPLEX ARM_SPI_MODE_MASTER_SIMPLEX 
#define ALP_SPI_MODE_SLAVE_SIMPLEX ARM_SPI_MODE_SLAVE_SIMPLEX 

/*----- SPI Control Codes: Mode Parameters: Frame Format -----*/
#define ALP_SPI_FRAME_FORMAT_Pos ARM_SPI_FRAME_FORMAT_Pos
#define ALP_SPI_FRAME_FORMAT_Msk ARM_SPI_FRAME_FORMAT_Msk
#define ALP_SPI_CPOL0_CPHA0 ARM_SPI_CPOL0_CPHA0
#define ALP_SPI_CPOL0_CPHA1 ARM_SPI_CPOL0_CPHA1
#define ALP_SPI_CPOL1_CPHA0 ARM_SPI_CPOL1_CPHA0
#define ALP_SPI_CPOL1_CPHA1 ARM_SPI_CPOL1_CPHA1
#define ALP_SPI_TI_SSI ARM_SPI_TI_SSI
#define ALP_SPI_MICROWIRE ARM_SPI_MICROWIRE

/*----- SPI Control Codes: Mode Parameters: Data Bits -----*/
#define ALP_SPI_DATA_BITS_Pos ARM_SPI_DATA_BITS_Pos
#define ALP_SPI_DATA_BITS_Msk ARM_SPI_DATA_BITS_Msk
#define ALP_SPI_DATA_BITS(n) ARM_SPI_DATA_BITS(n) 

/*----- SPI Control Codes: Mode Parameters: Bit Order -----*/
#define ALP_SPI_BIT_ORDER_Pos ARM_SPI_BIT_ORDER_Pos
#define ALP_SPI_BIT_ORDER_Msk ARM_SPI_BIT_ORDER_Msk
#define ALP_SPI_MSB_LSB ARM_SPI_MSB_LSB
#define ALP_SPI_LSB_MSB ARM_SPI_LSB_MSB

/*----- SPI Control Codes: Mode Parameters: Slave Select Mode -----*/
#define ALP_SPI_SS_MASTER_MODE_Pos ARM_SPI_SS_MASTER_MODE_Pos
#define ALP_SPI_SS_MASTER_MODE_Msk ARM_SPI_SS_MASTER_MODE_Msk
#define ALP_SPI_SS_MASTER_UNUSED ARM_SPI_SS_MASTER_UNUSED
#define ALP_SPI_SS_MASTER_SW ARM_SPI_SS_MASTER_SW
#define ALP_SPI_SS_MASTER_HW_OUTPUT ARM_SPI_SS_MASTER_HW_OUTPUT
#define ALP_SPI_SS_MASTER_HW_INPUT ARM_SPI_SS_MASTER_HW_INPUT
#define ALP_SPI_SS_SLAVE_MODE_Pos ARM_SPI_SS_SLAVE_MODE_Pos
#define ALP_SPI_SS_SLAVE_MODE_Msk ARM_SPI_SS_SLAVE_MODE_Msk
#define ALP_SPI_SS_SLAVE_HW ARM_SPI_SS_SLAVE_HW
#define ALP_SPI_SS_SLAVE_SW ARM_SPI_SS_SLAVE_SW

/*----- SPI Control Codes: Miscellaneous Controls  -----*/
#define ALP_SPI_SET_BUS_SPEED ARM_SPI_SET_BUS_SPEED 
#define ALP_SPI_GET_BUS_SPEED ARM_SPI_GET_BUS_SPEED
#define ALP_SPI_SET_DEFAULT_TX_VALUE ARM_SPI_SET_DEFAULT_TX_VALUE
#define ALP_SPI_CONTROL_SS ARM_SPI_CONTROL_SS
#define ALP_SPI_ABORT_TRANSFER ARM_SPI_ABORT_TRANSFER

/****** SPI Slave Select Signal definitions *****/
#define ALP_SPI_SS_INACTIVE ARM_SPI_SS_INACTIVE
#define ALP_SPI_SS_ACTIVE ARM_SPI_SS_ACTIVE

/****** SPI specific error codes *****/
#define ALP_SPI_ERROR_MODE ARM_SPI_ERROR_MODE
#define ALP_SPI_ERROR_FRAME_FORMAT ARM_SPI_ERROR_FRAME_FORMAT
#define ALP_SPI_ERROR_DATA_BITS ARM_SPI_ERROR_DATA_BITS
#define ALP_SPI_ERROR_BIT_ORDER ARM_SPI_ERROR_BIT_ORDER
#define ALP_SPI_ERROR_SS_MODE ARM_SPI_ERROR_SS_MODE

/****** SPI Event *****/
#define ALP_SPI_EVENT_TRANSFER_COMPLETE ARM_SPI_EVENT_TRANSFER_COMPLETE  ///< Data Transfer completed
#define ALP_SPI_EVENT_DATA_LOST         ARM_SPI_EVENT_DATA_LOST          ///< Data lost: Receive overflow / Transmit underflow
#define ALP_SPI_EVENT_MODE_FAULT        ARM_SPI_EVENT_MODE_FAULT         ///< Master Mode Fault (SS deactivated when Master)
/************************************
 * TYPEDEFS
 ************************************/
// TODO: cleanup and fix ALP event.
// typedef void (*ALP_SPI_SignalEvent_t) (uint32_t event);  ///< Pointer to \ref ARM_SPI_SignalEvent : Signal SPI Event.

/* Enumeration for SPI Instances */
typedef enum
{
    ALP_SPI_0,
    ALP_SPI_1
} ALP_SPI_Instance;

/**
\brief SPI Status
*/
typedef struct _ALP_SPI_STATUS {
  uint32_t busy       : 1;              ///< Transmitter/Receiver busy flag
  uint32_t data_lost  : 1;              ///< Data lost: Receive overflow / Transmit underflow (cleared on start of transfer operation)
  uint32_t mode_fault : 1;              ///< Mode fault detected; optional (cleared on start of transfer operation)
  uint32_t reserved   : 29;
} ALP_SPI_STATUS;

/**
\brief SPI Driver Capabilities.
*/
typedef struct _ALP_SPI_CAPABILITIES {
  uint32_t simplex          : 1;        ///< supports Simplex Mode (Master and Slave) @deprecated Reserved (must be zero)
  uint32_t ti_ssi           : 1;        ///< supports TI Synchronous Serial Interface
  uint32_t microwire        : 1;        ///< supports Microwire Interface
  uint32_t event_mode_fault : 1;        ///< Signal Mode Fault event: \ref ARM_SPI_EVENT_MODE_FAULT
  uint32_t reserved         : 28;       ///< Reserved (must be zero)
} ALP_SPI_CAPABILITIES;
/************************************
 * EXPORTED VARIABLES
 ************************************/

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
/**
 * @brief Get the version of the SPI driver.
 * 
 * @param spi_instance Denotes which SPI instance to get the version of.
 * 
 * @return ALP_DRIVER_VERSION The driver version information.
 */
ALP_DRIVER_VERSION ALP_SPI_GetVersion(ALP_SPI_Instance spi_instance);

/**
 * @brief Get the capabilities of the SPI driver.
 * 
 * @param spi_instance Denotes which SPI instance to get the capabilities of.
 * 
 * @return ALP_SPI_CAPABILITIES A structure describing the driver's capabilities.
 */
ALP_SPI_CAPABILITIES ALP_SPI_GetCapabilities(ALP_SPI_Instance spi_instance);

/**
 * @brief Initialize the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to initialize.
 * @param cb_event     Pointer to a callback function for signaling SPI events.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Initialize(ALP_SPI_Instance spi_instance, ARM_SPI_SignalEvent_t cb_event);

/**
 * @brief Uninitialize the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to uninitialize.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Uninitialize(ALP_SPI_Instance spi_instance);

/**
 * @brief Control the power state of the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to control.
 * @param state        The desired power state.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_PowerControl(ALP_SPI_Instance spi_instance, ARM_POWER_STATE state);

/**
 * @brief Send data using the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to use.
 * @param data         Pointer to the data to be sent.
 * @param num          Number of data items to send.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Send(ALP_SPI_Instance spi_instance, const void *data, uint32_t num);

/**
 * @brief Receive data using the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to use.
 * @param data         Pointer to the buffer to store received data.
 * @param num          Number of data items to receive.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Receive(ALP_SPI_Instance spi_instance, void *data, uint32_t num);

/**
 * @brief Perform a full-duplex transfer (simultaneous send and receive) using the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to use.
 * @param data_out     Pointer to the data to be sent.
 * @param data_in      Pointer to the buffer to store received data.
 * @param num          Number of data items to transfer.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Transfer(ALP_SPI_Instance spi_instance, const void *data_out, void *data_in, uint32_t num);

/**
 * @brief Get the number of data items transferred so far.
 * 
 * @param spi_instance The SPI instance to query.
 * 
 * @return uint32_t The number of data items transferred.
 */
uint32_t ALP_SPI_GetDataCount(ALP_SPI_Instance spi_instance);

/**
 * @brief Configure the specified SPI instance with control parameters.
 * 
 * @param spi_instance The SPI instance to configure.
 * @param control      Control command for the configuration.
 * @param arg          Argument for the control command.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Control(ALP_SPI_Instance spi_instance, uint32_t control, uint32_t arg);

/**
 * @brief Get the current status of the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to query.
 * 
 * @return ALP_SPI_STATUS The current status of the SPI instance.
 */
ALP_SPI_STATUS ALP_SPI_GetStatus(ALP_SPI_Instance spi_instance);


#ifdef __cplusplus
}
#endif

#endif /* ALP_SPI_H */
