/* Copyright (C) 2022 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

// Uncomment this to disable traces to UART
//#define DISABLE_UART_TRACE

#include "board.h"
#include "uart_tracelib.h"

#if !defined(DISABLE_UART_TRACE)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <RTE_Components.h>
#include CMSIS_device_header

/* UART Driver macro definitions */
#define _ARM_Driver_USART_(n)  Driver_USART##n
#define  ARM_Driver_USART_(n) _ARM_Driver_USART_(n)

/* UART Driver instance */
static ARM_DRIVER_USART *USARTdrv;

volatile uint32_t uart_event;
static bool initialized = false;
const char * tr_prefix = NULL;
static bool has_cb = false;
uint16_t prefix_len;
#define MAX_TRACE_LEN 256

int tracelib_init(const char * prefix, ARM_USART_SignalEvent_t cb_event)
{
    if (initialized)
    {
        return 0;
    }
    int32_t ret    = 0;
#if defined(M55_HE) || defined(M55_HE_E1C)
#if defined(CUSTOM_HE_UART)
    extern ARM_DRIVER_USART ARM_Driver_USART_(CUSTOM_HE_UART);
    USARTdrv = &ARM_Driver_USART_(CUSTOM_HE_UART);
#else
    extern ARM_DRIVER_USART ARM_Driver_USART_(BOARD_UART1_INSTANCE);
    USARTdrv = &ARM_Driver_USART_(BOARD_UART1_INSTANCE);
#endif
#elif defined(M55_HP)
    extern ARM_DRIVER_USART ARM_Driver_USART_(BOARD_UART2_INSTANCE);
    USARTdrv = &ARM_Driver_USART_(BOARD_UART2_INSTANCE);
#elif defined(A32)
    int cpuid = __get_MPIDR() & 0xFF;
    switch (cpuid) {
#ifdef BOARD_UART3_INSTANCE
    extern ARM_DRIVER_USART ARM_Driver_USART_(BOARD_UART3_INSTANCE);
    case 0:
        USARTdrv = &ARM_Driver_USART_(BOARD_UART3_INSTANCE);
        break;
#endif
#ifdef BOARD_UART4_INSTANCE
    extern ARM_DRIVER_USART ARM_Driver_USART_(BOARD_UART4_INSTANCE);
    case 1:
        USARTdrv = &ARM_Driver_USART_(BOARD_UART4_INSTANCE);
        break;
#endif
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
#else
    #error "Undefined CPU!"
#endif

    tr_prefix = prefix;
    if (tr_prefix)
    {
        prefix_len = strlen(tr_prefix);
    }
    else
    {
        prefix_len = 0;
    }

    /* Initialize UART driver */
    if (cb_event)
    {
        has_cb = true;
    }
    ret = USARTdrv->Initialize(cb_event);
    if (ret != ARM_DRIVER_OK)
    {
        return ret;
    }

    /* Power up UART peripheral */
    ret = USARTdrv->PowerControl(ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK)
    {
        return ret;
    }

    /* Configure UART to 115200 Bits/sec */
    ret =  USARTdrv->Control(ARM_USART_MODE_ASYNCHRONOUS |
                             ARM_USART_DATA_BITS_8       |
                             ARM_USART_PARITY_NONE       |
                             ARM_USART_STOP_BITS_1       |
                             ARM_USART_FLOW_CONTROL_NONE, 115200);
    if (ret != ARM_DRIVER_OK)
    {
        return ret;
    }

    /* Transmitter line */
    ret =  USARTdrv->Control(ARM_USART_CONTROL_TX, 1);
    if (ret != ARM_DRIVER_OK)
    {
        return ret;
    }

    /* Receiver line */
    ret =  USARTdrv->Control(ARM_USART_CONTROL_RX, 1);
    if (ret != ARM_DRIVER_OK)
    {
        return ret;
    }

    initialized = true;
    return ret;
}

int tracelib_uninit()
{
    int32_t ret = 0;
    if (initialized)
    {
        /* Power down UART peripheral */
        ret = USARTdrv->PowerControl(ARM_POWER_OFF);
        if (ret != ARM_DRIVER_OK)
        {
            return ret;
        }

        ret = USARTdrv->Uninitialize();
        if (ret != ARM_DRIVER_OK)
        {
            return ret;
        }

        initialized = false;
    }
    return ret;
}

int receive_str(char* str, uint32_t len)
{
    int ret = 0;
    if (initialized)
    {
        ret = USARTdrv->Receive(str, len);
        if (ret != ARM_DRIVER_OK)
        {
            return ret;
        }
        if (has_cb == false)
        {
            while (USARTdrv->GetRxCount() != len);
        }
    }
    return ret;
}

int send_str(const char* str, uint32_t len)
{
    int ret = 0;

    if (initialized)
    {
        uart_event = 0;
        ret = USARTdrv->Send(str, len);
        if (ret != ARM_DRIVER_OK)
        {
            return ret;
        }

        while (USARTdrv->GetTxCount() != len) __WFE();
    }
    return ret;
}

void vtracef(const char * format, va_list args)
{
    if (initialized)
    {
        static char buffer[MAX_TRACE_LEN];

        if (prefix_len) {
            memcpy(buffer, tr_prefix, prefix_len);
        }
        vsnprintf(buffer + prefix_len, sizeof(buffer) - prefix_len, format, args);
        send_str(buffer, strlen(buffer));
    }
}

void tracef(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    vtracef(format, args);
    va_end(args);
}

#else

int tracelib_init(const char * prefix, ARM_USART_SignalEvent_t cb_event)
{
    (void)prefix;
    (void)cb_event;
    return 0;
}

int tracelib_uninit()
{
    return 0;
}

int receive_str(char* str, uint32_t len)
{
    (void)str;
    (void)len;
    return 0;
}

int send_str(const char* str, uint32_t len)
{
    (void)str;
    (void)len;
    return 0;
}

void vtracef(const char * format, va_list args)
{
    (void)format;
    (void)args;
}

void tracef(const char * format, ...)
{
    (void)format;
}

#endif // DISABLE_UART_TRACE

/************************ (C) COPYRIGHT ALIF SEMICONDUCTOR *****END OF FILE****/
