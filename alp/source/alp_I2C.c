
#include "alp_I2C.h"
#include "stdint.h"
#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "Driver_I2C.h"
#include "pinconf.h"
#include "board.h"
#include "uart_tracelib.h"
#include "fault_handler.h"


extern ARM_DRIVER_I2C Driver_I2C1;
static ARM_DRIVER_I2C *I2C1_Drv = &Driver_I2C1;




extern ARM_DRIVER_I2C Driver_I2C0;
static ARM_DRIVER_I2C *I2C0_Drv = &Driver_I2C0;

ALP_Error_Type ALP_I2C_Init(I2C_Instance Port, uint8_t Role, I2CSpeed Speed, AdressBitValue AdressMode)
{
    ALP_Error_Type res;
     /// Select the I2C port 
     switch (Port)
     {
        case I2C_Port_0:
        {
            /* I2C0_SDA_A */
            pinconf_set(PORT_0, PIN_2, PINMUX_ALTERNATE_FUNCTION_3,
                    (PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP));

                /* I2C0_SCL_A */
            pinconf_set(PORT_0, PIN_3, PINMUX_ALTERNATE_FUNCTION_3,
            (PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP));
            res=I2C0_Drv->Initialize(NULL);
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }     

            res=I2C0_Drv->PowerControl(ARM_POWER_FULL); 
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }
            res = I2C0_Drv->Control(ARM_I2C_BUS_SPEED, Speed);
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }
            break;
        }
        case I2C_Port_1:
        {
            /* I2C1_SDA_C */
            pinconf_set(PORT_7, PIN_2, PINMUX_ALTERNATE_FUNCTION_5,
                (PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP));

            /* I2C1_SCL_C */
            pinconf_set(PORT_7, PIN_3, PINMUX_ALTERNATE_FUNCTION_5,
                (PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP));

            res=I2C1_Drv->Initialize(NULL);
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }     

            res=I2C1_Drv->PowerControl(ARM_POWER_FULL); 
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }
            res = I2C1_Drv->Control(ARM_I2C_BUS_SPEED, Speed);
            if(res!=ALP_DRIVER_OK)
            {
                return res;
            }       

            break;
        }
        default :
        {

            res=ALP_DRIVER_ERROR_PARAMETER;
            return res; 
        }
     }

     switch (Role)
     {
        case Master:
        {

            break;
        }
        case Slave: 
        {

            break;
        }    
     }
}


ALP_Error_Type ALP_I2C_DeInit(I2C_Instance Port)
{

    switch (Port )
    {
         case I2C_Port_0:
         {

             break;
         }
         case I2C_Port_1:
         {

             break;
         }       
    }

}


ALP_Error_Type ALP_I2C_MasterSendData(I2C_Instance Port,uint16_t Adress, AdressBitValue AdressMode,uint8_t *data2send, uint16_t len)
{


}
ALP_Error_Type ALP_I2C_MasterReceiveData(I2C_Instance Port,uint16_t Adress,AdressBitValue AdressMode,uint8_t *data2send, uint16_t len)
{


}

ALP_Error_Type ALP_I2C_SlaveSendData(I2C_Instance Port,uint8_t *data2send, uint16_t len)
{
    ALP_Error_Type res;
    switch (Port)
    {
        case I2C_Port_0:
        {
            res = I2C0_Drv->SlaveTransmit(data2send, len);
            if (res != ALP_DRIVER_OK)
            {
               return res; 
            }
            break; 
        }
        case I2C_Port_1:
        {
            res = I2C1_Drv->SlaveTransmit(data2send, len);
            if (res != ALP_DRIVER_OK)
            {
               return res; 
            }
            break;  
        }        
    }
    return res; 

}



ALP_Error_Type ALP_I2C_SlaveReceiveData(I2C_Instance Port,uint8_t *data2receive, uint16_t len)
{
    ALP_Error_Type res;
    switch (Port)
    {
        case I2C_Port_0:
        {
            res = I2C0_Drv->SlaveReceive(data2receive, len);
            if (res != ALP_DRIVER_OK)
            {
               return res; 
            }
            break; 
        }
        case I2C_Port_1:
        {
             res = I2C1_Drv->SlaveReceive(data2receive, len);
            if (res != ALP_DRIVER_OK)
            {
               return res; 
            }
            break;  
        }        
    }
    return res; 
}

// will be used only in the Slave Mode 
ALP_Error_Type ALP_I2C_SetOwnAddress(I2C_Instance Port,uint8_t Address,AdressBitValue AdressMode)
{
    ALP_Error_Type res;
    switch (Port)
    {
        case I2C_Port_0:
        {

            if (AdressMode == ADDRESS_MODE_10BIT)
            {

                res = I2C0_Drv->Control(ARM_I2C_OWN_ADDRESS, (Address | ADDRESS_MODE_10BIT));
            }
            else
            {
                res = I2C0_Drv->Control(ARM_I2C_OWN_ADDRESS, Address);
            }                
            break; 
        } 
        case I2C_Port_1:
        {
            if (AdressMode == ADDRESS_MODE_10BIT)
            {

                res = I2C1_Drv->Control(ARM_I2C_OWN_ADDRESS, (Address | ADDRESS_MODE_10BIT));
            }
            else
            {
                res = I2C1_Drv->Control(ARM_I2C_OWN_ADDRESS, Address);
            }     
            break; 
        } 
    }
    return res;
}