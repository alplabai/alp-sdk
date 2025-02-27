/**
 ********************************************************************************
 * @file    alp_driver_common.h
 * @author  Sukru Aydogdu
 * @date    16/1/2025
 * @brief   Common file for ALP Drivers.
 ********************************************************************************
 */

#ifndef ALP_DRIVER_COMMON_H
#define ALP_DRIVER_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/************************************
 * INCLUDES
 ************************************/
#include "stdint.h"
#include <stddef.h>
#include <stdbool.h>
#include "Driver_Common.h" // ALIF driver common

/************************************
 * MACROS AND DEFINES
 ************************************/
//TODO: this file should be used eventually for all the status reports.
#define ALP_DRIVER_VERSION_MAJOR_MINOR(major,minor) (((major) << 8) | (minor))

/* General return codes */
#define ALP_DRIVER_OK                 0 ///< Operation succeeded 
#define ALP_DRIVER_ERROR             -1 ///< Unspecified error
#define ALP_DRIVER_ERROR_BUSY        -2 ///< Driver is busy
#define ALP_DRIVER_ERROR_TIMEOUT     -3 ///< Timeout occurred
#define ALP_DRIVER_ERROR_UNSUPPORTED -4 ///< Operation not supported
#define ALP_DRIVER_ERROR_PARAMETER   -5 ///< Parameter error
#define ALP_DRIVER_ERROR_SPECIFIC    -6 ///< Start of driver specific errors

/************************************
 * TYPEDEFS
 ************************************/
typedef struct _ALP_DRIVER_VERSION {
  uint16_t api;                         ///< API version
  uint16_t drv;                         ///< Driver version
} ALP_DRIVER_VERSION;

/**
\brief General power states
*/ 
typedef enum _ALP_POWER_STATE {
  ALP_POWER_OFF,                        ///< Power off: no operation possible
  ALP_POWER_LOW,                        ///< Low Power mode: retain state, detect and signal wake-up events
  ALP_POWER_FULL                        ///< Power on: full operation at maximum performance
} ALP_POWER_STATE;

/************************************
 * EXPORTED VARIABLES
 ************************************/
// Add any extern variable declarations if necessary

/************************************
 * GLOBAL FUNCTION PROTOTYPES
 ************************************/
/**
 * @brief Convert version data to ALP_DRIVER_VERSION
 * 
 * @param version Version data
 * 
 * @return ALP_DRIVER_VERSION Converted ALP_DRIVER_VERSION
 */
ALP_DRIVER_VERSION ALP_ConvertVersion(ARM_DRIVER_VERSION version);

#ifdef __cplusplus
}
#endif

#endif /* ALP_DRIVER_COMMON_H */
