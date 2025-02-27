/**
 ********************************************************************************
 * @file    alp_spi_example.c
 * @author  Your name
 * @date    31/10/2024
 * @brief   Alp SPI example file
 ********************************************************************************

/*
* H/W connections on Alif Ensembe E7 development kit:
* short SPI0 MISO (P5_0 -> J12-13 pin) and SPI1 MISO (P8_3 -> J14-15 pin).
* short SPI0 MOSI (P5_1 -> J12-15 pin) and SPI1 MOSI (P8_4 -> J14-17 pin).
* short SPI0 SCLK (P5_3 -> J14-5 pin) and SPI1 SCLK (P8_5 -> J14-19 pin).
* short SPI0 SS (P5_2 -> J12-17 pin) and SPI1 SS (P6_4 -> J12-22 pin).
*/

/************************************
 * INCLUDES
 ************************************/
#include <stdio.h>
#include "Driver_SPI.h"
#include "pinconf.h"
#include "RTE_Components.h"

#if defined(RTE_Compiler_IO_STDOUT)
#include "retarget_stdout.h"
#endif  /* RTE_Compiler_IO_STDOUT */

#include "board.h"
#include "uart_tracelib.h"
#include "fault_handler.h"
#include "system_utils.h"
#include "alp_spi.h"
/************************************
 * EXTERN VARIABLES
 ************************************/
// Add extern variable declarations if necessary

/************************************
 * PRIVATE MACROS AND DEFINES
 ************************************/
#define FULL_DUPLEX_EXAMPLE  0
#define PRINT_VERSION_INFO   1
#define PRINT_CAPABILITIES   1

/************************************
 * PRIVATE TYPEDEFS
 ************************************/

/************************************
 * STATIC VARIABLES
 ************************************/

/************************************
 * GLOBAL VARIABLES
 ************************************/
volatile uint8_t spi1_cb_status = 0;
volatile uint8_t spi0_cb_status = 0;

/************************************
 * STATIC FUNCTION PROTOTYPES
 ************************************/

/************************************
 * STATIC FUNCTIONS
 ************************************/
static void uart_callback(uint32_t event)
{
    // do nothing
}

/**
 * @fn      static int32_t pinmux_config(void)
 * @brief   SPI1 & SPI0 pinmux configuration with UART2 settings.
 * @note    none.
 * @param   none.
 * @retval  execution status.
 */
static int32_t pinmux_config(void)
{
    int32_t ret = ALP_DRIVER_OK;

    // pin config for UART2
	ret = pinconf_set(PORT_1, PIN_0, PINMUX_ALTERNATE_FUNCTION_1, PADCTRL_READ_ENABLE | PADCTRL_SCHMITT_TRIGGER_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP);
    if(ret)
    {
        printf("ERROR: Failed to configure PINMUX for UART2_RX_PIN\n");
        return ret;
    }
	
    ret = pinconf_set(PORT_1, PIN_1, PINMUX_ALTERNATE_FUNCTION_1, 0);
    if(ret)
    {
        printf("ERROR: Failed to configure PINMUX for UART2_TX_PIN\n");
        return ret;
    }

    /* pinmux configurations for SPI0 pins (using B version pins) */
    ret = pinconf_set(PORT_5, PIN_0, PINMUX_ALTERNATE_FUNCTION_4, PADCTRL_READ_ENABLE);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI0_MISO_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_5, PIN_1, PINMUX_ALTERNATE_FUNCTION_4, 0);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI0_MOSI_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_5, PIN_3, PINMUX_ALTERNATE_FUNCTION_3, 0);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI0_CLK_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_5, PIN_2, PINMUX_ALTERNATE_FUNCTION_4, 0);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI0_SS_PIN\n");
        return ret;
    }

    /* pinmux configurations for SPI1 pins (using B version pins) */
    ret = pinconf_set(PORT_8, PIN_3, PINMUX_ALTERNATE_FUNCTION_2, 0);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI1_MISO_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_8, PIN_4, PINMUX_ALTERNATE_FUNCTION_2, PADCTRL_READ_ENABLE);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI1_MOSI_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_8, PIN_5, PINMUX_ALTERNATE_FUNCTION_2, PADCTRL_READ_ENABLE);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI1_CLK_PIN\n");
        return ret;
    }
    ret = pinconf_set(PORT_6, PIN_4, PINMUX_ALTERNATE_FUNCTION_4, PADCTRL_READ_ENABLE);
    if (ret)
    {
        printf("ERROR: Failed to configure PINMUX for SPI1_SS_PIN\n");
        return ret;
    }

    return ret;
}

/**
 * @fn      static void SPI0_cb_func (uint32_t event)
 * @brief   SPI0 callback function.
 * @note    none.
 * @param   event: SPI event.
 * @retval  none.
 */
static void SPI0_cb_func (uint32_t event)
{
    if (event == ALP_SPI_EVENT_TRANSFER_COMPLETE)
    {
        spi0_cb_status = 1;
    }
}

/**
 * @fn      static void SPI1_cb_func (uint32_t event)
 * @brief   SPI1 callback function.
 * @note    none.
 * @param   event: SPI event.
 * @retval  none.
 */
static void SPI1_cb_func (uint32_t event)
{
    if (event == ALP_SPI_EVENT_TRANSFER_COMPLETE)
    {
        spi1_cb_status = 1;
    }
}

/**
 * @fn      static void spi0_spi1_transfer(void)
 * @brief   demo application function for data transfer.
 * @note    none.
 * @param   none.
 * @retval  none.
 */
static void spi0_spi1_transfer(void)
{
    uint32_t spi0_tx_buff, spi1_rx_buff = 0;
    int32_t ret = ALP_DRIVER_OK;
    uint32_t spi1_control, spi0_control;
#if FULL_DUPLEX_EXAMPLE
    uint32_t spi1_tx_buff, spi0_rx_buff = 0;
#endif

    ret = pinmux_config();
    if (ret != ALP_DRIVER_OK)
    {
        printf("Error in pinmux configuration\n");
        return;
    }
    
    printf("*** ALP SPI Data transfer example using SPI0 & SPI1 is starting ***\n");

    /* SPI0 Configuration as master */
    ret = ALP_SPI_Initialize(ALP_SPI_0, SPI0_cb_func);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to initialize the SPI0\n");
        return;
    }
    
    # if PRINT_VERSION_INFO
        ALP_DRIVER_VERSION spi_0_ver = ALP_SPI_GetVersion(ALP_SPI_0);
        ALP_DRIVER_VERSION spi_1_ver = ALP_SPI_GetVersion(ALP_SPI_1);
        printf("SPI0 Driver version: %d.%d\n", spi_0_ver.api, spi_0_ver.drv);
        printf("SPI1 Driver version: %d.%d\n", spi_1_ver.api, spi_1_ver.drv);
    #endif

    #if PRINT_CAPABILITIES
        ALP_SPI_CAPABILITIES spi_0_cap = ALP_SPI_GetCapabilities(ALP_SPI_0);
        ALP_SPI_CAPABILITIES spi_1_cap = ALP_SPI_GetCapabilities(ALP_SPI_1);
        printf("SPI0 Capabilities: %d\n", spi_0_cap);
        printf("SPI1 Capabilities: %d\n", spi_1_cap);
    #endif

    ret = ALP_SPI_PowerControl(ALP_SPI_0, ALP_POWER_FULL);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to power SPI0\n");
        goto error_spi0_uninitialize;
    }

    spi0_control = (ALP_SPI_MODE_MASTER | ALP_SPI_SS_MASTER_HW_OUTPUT | ALP_SPI_CPOL0_CPHA0 | ALP_SPI_DATA_BITS(32));

    /* Baudrate is 1MHz */
    ret = ALP_SPI_Control(ALP_SPI_0, spi0_control, 1000000);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to configure SPI0\n");
        goto error_spi0_power_off;
    }

    /* SPI1 Configuration as slave */
    ret = ALP_SPI_Initialize(ALP_SPI_1, SPI1_cb_func);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to initialize the SPI1\n");
        goto error_spi0_power_off;
    }

    ret = ALP_SPI_PowerControl(ALP_SPI_1, ALP_POWER_FULL);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to power SPI1\n");
        goto error_spi1_uninitialize;
    }

    spi1_control = (ALP_SPI_MODE_SLAVE | ALP_SPI_CPOL0_CPHA0 | ALP_SPI_DATA_BITS(32));

    ret = ALP_SPI_Control(ALP_SPI_1, spi1_control, (uint32_t)NULL);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to configure SPI1\n");
        goto error_spi1_power_off;
    }

    ret = ALP_SPI_Control(ALP_SPI_1, ALP_SPI_CONTROL_SS, ALP_SPI_SS_ACTIVE);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed to enable the slave select of SPI0\n");
        goto error_spi1_power_off;
    }

#if FULL_DUPLEX_EXAMPLE
    spi0_tx_buff = 0xAAAAAAAA;
    spi1_tx_buff = 0x55555555;

    ret = ALP_SPI_Transfer(ALP_SPI_1, &spi1_tx_buff, &spi1_rx_buff, 1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed SPI1 to configure as tx_rx\n");
        goto error_spi1_power_off;
    }

    ret = ALP_SPI_Transfer(ALP_SPI_0, &spi0_tx_buff, &spi0_rx_buff, 1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: Failed SPI0 to configure as tx_rx\n");
        goto error_spi1_power_off;
    }

#else
    spi0_tx_buff = 0x12345678;

    ret = ALP_SPI_Receive(ALP_SPI_1, &spi1_rx_buff, 1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: SPI1 Failed to configure as receive only\n");
        goto error_spi1_power_off;
    }

    ret = ALP_SPI_Send(ALP_SPI_0, &spi0_tx_buff, 1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR: SPI0 Failed to configure as send only\n");
        goto error_spi1_power_off;
    }
#endif

    while(1)
    {
        if (spi0_cb_status && spi1_cb_status)
        {
            spi0_cb_status = 0;
            spi1_cb_status = 0;
            break;
        }
    }

    while (!((ALP_SPI_GetStatus(ALP_SPI_0).busy == 0) && (ALP_SPI_GetStatus(ALP_SPI_1).busy == 0)));
    printf("Data Transfer completed\n");

    printf("SPI1 received value : 0x%x\n", spi1_rx_buff);
#if FULL_DUPLEX_EXAMPLE
    printf("SPI0 received value : 0x%x\n", spi0_rx_buff);
#endif

error_spi1_power_off :

    ret = ALP_SPI_PowerControl(ALP_SPI_1, ALP_POWER_OFF);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI1 power off\n");
    }

error_spi1_uninitialize :
    ret = ALP_SPI_Uninitialize(ALP_SPI_1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI1 un-initialization\n");
    }

error_spi0_power_off :
    ret = ALP_SPI_PowerControl(ALP_SPI_0, ALP_POWER_OFF);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI0 power off\n");
    }

error_spi0_uninitialize :
    ret = ALP_SPI_Uninitialize(ALP_SPI_0);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI0 un-initialization\n");
    }

    #if 0
    uint32_t spi_0_data_count = ALP_SPI_GetDataCount(ALP_SPI_0);
    uint32_t spi_1_data_count = ALP_SPI_GetDataCount(ALP_SPI_1);
    printf("SPI0 Data count: %d\n", spi_0_data_count);
    printf("SPI1 Data count: %d\n", spi_1_data_count);
    #endif

    printf("*** ALP SPI Data transfer example using SPI0 & SPI1 is ended ***\n");
    
    // Uninitialize the SPI instances
    ret = ALP_SPI_Uninitialize(ALP_SPI_0);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI0 un-initialization\n");
    }
    
    ret = ALP_SPI_Uninitialize(ALP_SPI_1);
    if (ret != ALP_DRIVER_OK)
    {
        printf("ERROR in SPI1 un-initialization\n");
    }
}
/************************************
 * MAIN FUNCTION
 ************************************/

int main()
{
    sys_busy_loop_init();

    // Use common_app_utils for printing
    tracelib_init(NULL, uart_callback);

    fault_dump_enable(true);
    
    spi0_spi1_transfer();
}
