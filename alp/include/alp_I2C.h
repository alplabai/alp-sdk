
/**
 ********************************************************************************
 * @file    alp_I2C.h
 * @author  Gurkan Kucukyildiz
 * @date    31/10/2024
 * @brief   Header file for ALP I2C configuration functions
 ********************************************************************************
 */

#ifndef ALP_GPIO_H
#define ALP_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include "stdint.h"
#include "Driver_I2C.h"

/************************************
 * MACROS AND DEFINES
 ************************************/

typedef enum
{
    Master, //0
    Slave,  //1 
} I2CMode;

typedef enum 
{
 ALP_DRIVER_OK     =            0, ///< Operation succeeded 
 ALP_DRIVER_ERROR   =          -1 ,///< Unspecified error
 ALP_DRIVER_ERROR_BUSY  =       -2 ,///< Driver is busy
 ALP_DRIVER_ERROR_TIMEOUT  =    -3 ,///< Timeout occurred
 ALP_DRIVER_ERROR_UNSUPPORTED = -4 ,///< Operation not supported
 ALP_DRIVER_ERROR_PARAMETER   = -5 ,///< Parameter error
 ALP_DRIVER_ERROR_SPECIFIC  =  -6 ///< Start of driver specific errors 
}ALP_Error_Type; 

typedef enum
{
    ADDRESS_MODE_7BIT, //0
    ADDRESS_MODE_10BIT, // 1
} AdressBitValue;

// Unsupported
typedef enum
{
   Speed100K, // 0
   Speed400K, // 1
   Speed1M, // 2
   Speed3M4, // 3
   UnsupportedSpeed, //4 
}I2CSpeed; 

typedef enum 
{
   I2C_Port_0,//0
   I2C_Port_1, //1
   UnsupportedPort, // 2 
}I2C_Instance; 

typedef enum
{
    Event_I2C_CB_EVENT_TRANSFER_DONE,       // 0
    Event_I2C_CB_EVENT_ADDRESS_NACK,        // 1
    Event_I2C_CB_EVENT_TRANSFER_INCOMPLETE, // 2 
}I2CEventType;


/************************************
 * EXPORTED VARIABLES
 ************************************/
// Add any extern variable declarations if necessary

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
ALP_Error_Type ALP_I2C_Init(I2C_Instance Port, I2CMode Role, I2CSpeed Speed, AdressBitValue AdressMode);
ALP_Error_Type ALP_I2C_SetOwnAddress(I2C_Instance Port,uint8_t Address,AdressBitValue AdressMode);
ALP_Error_Type ALP_I2C_DeInit(I2C_Instance Port);

ALP_Error_Type ALP_I2C_EnableInterrupt(I2C_Instance Port,I2CEventType event);
ALP_Error_Type ALP_I2C_DisableInterrupt(I2C_Instance Port,I2CEventType event);

// Communication Functions
ALP_Error_Type ALP_I2C_MasterSendData(I2C_Instance Port,uint16_t Adress, AdressBitValue AdressMode,uint8_t *data2send, uint16_t len);
ALP_Error_Type ALP_I2C_MasterReceiveData(I2C_Instance Port,uint16_t Adress,AdressBitValue AdressMode,uint8_t *data2send, uint16_t len); 

ALP_Error_Type ALP_I2C_SlaveSendData(I2C_Instance Port,uint8_t *data2send, uint16_t len);
ALP_Error_Type ALP_I2C_SlaveReceiveData(I2C_Instance Port,uint8_t *data2receive, uint16_t len);
#ifdef __cplusplus
}
#endif

#endif /* alp_I2C_H */









