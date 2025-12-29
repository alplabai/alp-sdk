/* Copyright (C) 2022 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#ifndef UART_TRACELIB_H_
#define UART_TRACELIB_H_

#include <stdarg.h>
#include <stdint.h>
#include "Driver_USART.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the trace lib.
 *
 * @param prefix string prepended to every trace message
 *
 * @note Max length is currently 256 - length of the prefix
 */
int tracelib_init(const char * prefix, ARM_USART_SignalEvent_t cb_event);

/**
 * @brief Uninitializes the trace lib.
 */
int tracelib_uninit();

/**
 * @brief write trace to UART
 */
void tracef(const char * format, ...);
void vtracef(const char * format, va_list args);

/**
 * @brief Receive string from UART.
 *
 * @param str string buffer for received data
 * @param len maximum length of the string
 */
int receive_str(char* str, uint32_t len);

/**
 * @brief Send string to UART, no prefix is prepended.
 *
 * @param str string to send over UART
 * @param len length of the string
 */
int send_str(const char* str, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* UART_TRACELIB_H_ */
