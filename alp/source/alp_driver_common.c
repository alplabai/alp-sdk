/**
 ********************************************************************************
 * @file    alp_driver_common.c
 * @author  Sukru Aydogdu
 * @date    26/01/2025
 * @brief   Alp driver common functions
 ********************************************************************************
 

/************************************
 * INCLUDES
 ************************************/
#include "alp_driver_common.h"

/************************************
 * EXTERN VARIABLES
 ************************************/
// Add extern variable declarations if necessary

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
// Define any private macros or constants if needed

/************************************
 * PRIVATE TYPEDEFS
 ************************************/
// Define private typedefs if needed

/************************************
 * STATIC VARIABLES
 ************************************/
// Declare static (file-scoped) variables if needed

/************************************
 * GLOBAL VARIABLES
 ************************************/
// Declare any global variables if necessary

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/
// Declare static (private) function prototypes if needed

/************************************
 * STATIC FUNCTIONS
 ************************************/
// Implement static (private) functions if needed

/************************************
 * GLOBAL FUNCTIONS
 ************************************/
/**
 * @brief Convert version data to ALP_DRIVER_VERSION
 * 
 * @param version Version data
 * 
 * @return ALP_DRIVER_VERSION Converted ALP_DRIVER_VERSION
 */
ALP_DRIVER_VERSION ALP_ConvertVersion(ARM_DRIVER_VERSION version)
{
    ALP_DRIVER_VERSION alp_version;

    // map version data
    alp_version.api = version.api;
    alp_version.drv = version.drv;

    return alp_version;
}
