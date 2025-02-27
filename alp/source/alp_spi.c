/**
 ********************************************************************************
 * @file    alp_spi.c
 * @author  Sukru Aydogdu
 * @date    16/01/2025
 * @brief   Implementation of ALP SPI functions
 ********************************************************************************
 

/************************************
 * INCLUDES
 ************************************/
#include "alp_spi.h"

/************************************
 * EXTERN VARIABLES
 ************************************/
extern ARM_DRIVER_SPI Driver_SPI0;
extern ARM_DRIVER_SPI Driver_SPI1;

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/

/************************************
 * PRIVATE TYPEDEFS
 ************************************/

/************************************
 * STATIC VARIABLES
 ************************************/
static ARM_DRIVER_SPI *alp_spi_0 = &Driver_SPI0;
static ARM_DRIVER_SPI *alp_spi_1 = &Driver_SPI1;

/************************************
 * GLOBAL VARIABLES
 ************************************/

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/

/************************************
 * STATIC FUNCTIONS
 ************************************/
/**
 * @brief Convert spi status data to ALP_SPI_STATUS
 * 
 * @param spi_status The spi status data to convert.
 * 
 * @return ALP_SPI_STATUS The converted ALP_SPI_STATUS data.
 */
ALP_SPI_STATUS ConvertSPIStatus(ARM_SPI_STATUS spi_status)
{
   ALP_SPI_STATUS alp_status;

   // map status data
   alp_status.busy = spi_status.busy;
   alp_status.data_lost = spi_status.data_lost;
   alp_status.mode_fault = spi_status.mode_fault;

   // copy reserved bits
   alp_status.reserved = spi_status.reserved;
}

/**
 * @brief Convert spi capabilities data to ALP_SPI_CAPABILITIES
 * 
 * @param spi_status The spi capabilities data to convert.
 * 
 * @return ALP_SPI_CAPABILITIES The converted ALP_SPI_CAPABILITIES data.
 */
ALP_SPI_CAPABILITIES ConvertSPICapabilities(ARM_SPI_CAPABILITIES spi_capabilities)
{
   ALP_SPI_CAPABILITIES alp_capabilities;

   // map capabilities data
   alp_capabilities.simplex = spi_capabilities.simplex;
   alp_capabilities.ti_ssi = spi_capabilities.ti_ssi;
   alp_capabilities.microwire = spi_capabilities.microwire;
   alp_capabilities.event_mode_fault = spi_capabilities.event_mode_fault;

   // copy reserved bits
   alp_capabilities.reserved = spi_capabilities.reserved;
}
/************************************
 * GLOBAL FUNCTIONS
 ************************************/
/**
 * @brief Get the version of the SPI driver.
 * 
 * @return ALP_DRIVER_VERSION The driver version information.
 */
ALP_DRIVER_VERSION ALP_SPI_GetVersion(ALP_SPI_Instance spi_instance_no)
{
   switch (spi_instance_no)
   {
   case ALP_SPI_0:
         return ALP_ConvertVersion(alp_spi_0->GetVersion());
      break;
   case ALP_SPI_1:
         return ALP_ConvertVersion(alp_spi_1->GetVersion());
      break;
   default:
      break;
   }
}

/**
 * @brief Get the capabilities of the SPI driver.
 * 
 * @return ALP_SPI_CAPABILITIES A structure describing the driver's capabilities.
 */
ALP_SPI_CAPABILITIES ALP_SPI_GetCapabilities(ALP_SPI_Instance spi_instance_no)
{
   switch (spi_instance_no)
   {
   case ALP_SPI_0:
         return ConvertSPICapabilities(alp_spi_0->GetCapabilities());
      break;
   case ALP_SPI_1:
         return ConvertSPICapabilities(alp_spi_1->GetCapabilities());
      break;
   default:
      break;
   }
}

/**
 * @brief Initialize the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to initialize.
 * @param cb_event     Pointer to a callback function for signaling SPI events.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Initialize(ALP_SPI_Instance spi_instance, ARM_SPI_SignalEvent_t cb_event)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Initialize(cb_event);
      break;
   case ALP_SPI_1:
         return alp_spi_1->Initialize(cb_event);
      break;
   default:
      break;
   }
}

/**
 * @brief Uninitialize the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to uninitialize.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Uninitialize(ALP_SPI_Instance spi_instance)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Uninitialize();
      break;
   case ALP_SPI_1:
         return alp_spi_1->Uninitialize();
      break;
   default:
      break;
   }
}

/**
 * @brief Control the power state of the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to control.
 * @param state        The desired power state.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_PowerControl(ALP_SPI_Instance spi_instance, ARM_POWER_STATE state)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->PowerControl(state);
      break;
   case ALP_SPI_1:
         return alp_spi_1->PowerControl(state);
      break;
   default:
      break;
   }
}

/**
 * @brief Send data using the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to use.
 * @param data         Pointer to the data to be sent.
 * @param num          Number of data items to send.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Send(ALP_SPI_Instance spi_instance, const void *data, uint32_t num)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Send(data, num);
      break;
   case ALP_SPI_1:
         return alp_spi_1->Send(data, num);
      break;
   default:
      break;
   }
}

/**
 * @brief Receive data using the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to use.
 * @param data         Pointer to the buffer to store received data.
 * @param num          Number of data items to receive.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Receive(ALP_SPI_Instance spi_instance, void *data, uint32_t num)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Receive(data, num);
      break;
   case ALP_SPI_1:
         return alp_spi_1->Receive(data, num);
      break;
   default:
      break;
   }
}

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
int32_t ALP_SPI_Transfer(ALP_SPI_Instance spi_instance, const void *data_out, void *data_in, uint32_t num)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Transfer(data_out, data_in, num);
      break;
   case ALP_SPI_1:
         return alp_spi_1->Transfer(data_out, data_in, num);
      break;
   default:
      break;
   }
}

/**
 * @brief Get the number of data items transferred so far.
 * 
 * @param spi_instance The SPI instance to query.
 * 
 * @return uint32_t The number of data items transferred.
 */
uint32_t ALP_SPI_GetDataCount(ALP_SPI_Instance spi_instance)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->GetDataCount();
      break;
   case ALP_SPI_1:
         return alp_spi_1->GetDataCount();
      break;
   default:
      break;
   }
}

/**
 * @brief Configure the specified SPI instance with control parameters.
 * 
 * @param spi_instance The SPI instance to configure.
 * @param control      Control command for the configuration.
 * @param arg          Argument for the control command.
 * 
 * @return int32_t 0 on success, or a negative value indicating an error.
 */
int32_t ALP_SPI_Control(ALP_SPI_Instance spi_instance, uint32_t control, uint32_t arg)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return alp_spi_0->Control(control, arg);
      break;
   case ALP_SPI_1:
         return alp_spi_1->Control(control, arg);
      break;
   default:
      break;
   }
}

/**
 * @brief Get the current status of the specified SPI instance.
 * 
 * @param spi_instance The SPI instance to query.
 * 
 * @return ALP_SPI_STATUS The current status of the SPI instance.
 */
ALP_SPI_STATUS ALP_SPI_GetStatus(ALP_SPI_Instance spi_instance)
{
   switch (spi_instance)
   {
   case ALP_SPI_0:
         return ConvertSPIStatus(alp_spi_0->GetStatus());
      break;
   case ALP_SPI_1:
         return ConvertSPIStatus(alp_spi_1->GetStatus());
      break;
   default:
      break;
   }
}
