/**
 ********************************************************************************
 * @file    alp_adc.c
 * @author  Sukru Aydogdu
 * @date    31/01/2025
 * @brief   Alp ADC Driver implementation
 ********************************************************************************
 

/************************************
 * INCLUDES
 ************************************/
#include "alp_adc.h"

/************************************
 * EXTERN VARIABLES
 ************************************/
extern ARM_DRIVER_ADC Driver_ADC120;
extern ARM_DRIVER_ADC Driver_ADC121;
extern ARM_DRIVER_ADC Driver_ADC122;
extern ARM_DRIVER_ADC Driver_ADC24;

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
static ARM_DRIVER_ADC *alp_adc_0 = &Driver_ADC120;
static ARM_DRIVER_ADC *alp_adc_1 = &Driver_ADC121;
static ARM_DRIVER_ADC *alp_adc_2 = &Driver_ADC122;
static ARM_DRIVER_ADC *alp_adc_3 = &Driver_ADC24; // 24 bit ADC of ALif Ensemble E7

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
ARM_DRIVER_VERSION ALP_ADC_GetVersion(ALP_ADC_Instance adc_instance)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->GetVersion();
        break;
    case ALP_ADC_1:
        alp_adc_1->GetVersion();
        break;
    case ALP_ADC_2:
        alp_adc_2->GetVersion();
        break;
    case ALP_ADC_3:
        alp_adc_3->GetVersion();
        break;
    default:
        break;
    }
}

ARM_ADC_CAPABILITIES ALP_ADC_GetCapabilities(ALP_ADC_Instance adc_instance)
{ 
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->GetCapabilities();
        break;
    case ALP_ADC_1:
        alp_adc_1->GetCapabilities();
        break;
    case ALP_ADC_2:
        alp_adc_2->GetCapabilities();
        break;
    case ALP_ADC_3:
        alp_adc_3->GetCapabilities();
        break;
    default:
        break;
    }   
}

int32_t ALP_ADC_Initialize(ALP_ADC_Instance adc_instance,ARM_ADC_SignalEvent_t cb_event)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->Initialize(cb_event);
        break;
    case ALP_ADC_1:
        alp_adc_1->Initialize(cb_event);
        break;
    case ALP_ADC_2:
        alp_adc_2->Initialize(cb_event);
        break;
    case ALP_ADC_3:
        alp_adc_3->Initialize(cb_event);
        break;
    default:
        break;
    }
}

int32_t ALP_ADC_Uninitialize(ALP_ADC_Instance adc_instance)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->Uninitialize();
        break;
    case ALP_ADC_1:
        alp_adc_1->Uninitialize();
        break;
    case ALP_ADC_2:
        alp_adc_2->Uninitialize();
        break;
    case ALP_ADC_3:
        alp_adc_3->Uninitialize();
        break;
    default:
        break;
    }
}

int32_t ALP_ADC_Start(ALP_ADC_Instance adc_instance)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->Start();
        break;
    case ALP_ADC_1:
        alp_adc_1->Start();
        break;
    case ALP_ADC_2:
        alp_adc_2->Start();
        break;
    case ALP_ADC_3:
        alp_adc_3->Start();
        break;
    default:
        break;
    }
}

int32_t ALP_ADC_Stop(ALP_ADC_Instance adc_instance)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->Stop();
        break;
    case ALP_ADC_1:
        alp_adc_1->Stop();
        break;
    case ALP_ADC_2:
        alp_adc_2->Stop();
        break;
    case ALP_ADC_3:
        alp_adc_3->Stop();
        break;
    default:
        break;
    }
}

int32_t ALP_ADC_PowerControl(ALP_ADC_Instance adc_instance, ARM_POWER_STATE state)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->PowerControl(state);
        break;
    case ALP_ADC_1:
        alp_adc_1->PowerControl(state);
        break;
    case ALP_ADC_2:
        alp_adc_2->PowerControl(state);
        break;
    case ALP_ADC_3:
        alp_adc_3->PowerControl(state);
        break;
    default:
        break;
    }
}

int32_t ALP_ADC_Control(ALP_ADC_Instance adc_instance, uint32_t Control, uint32_t arg)
{
    switch (adc_instance)
    {
    case ALP_ADC_0:
        alp_adc_0->Control(Control,arg);
        break;
    case ALP_ADC_1:
        alp_adc_1->Control(Control,arg);
        break;
    case ALP_ADC_2:
        alp_adc_2->Control(Control,arg);
        break;
    case ALP_ADC_3:
        alp_adc_3->Control(Control,arg);
        break;
    default:
        break;
    }
}
