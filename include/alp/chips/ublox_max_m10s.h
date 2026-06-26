/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ublox_max_m10s.h
 * @brief u-blox MAX-M10S small-footprint GNSS receiver (UART NMEA).
 *
 * Compact (10.1 × 9.7 mm) successor to MAX-M8S/MAX-M9.  Same UART
 * NMEA shape as `ublox_neo_m9n`; this driver provides the same
 * thin line-reader.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: u-blox MAX-M10S v1.4 (Sep 2022).
 */

#ifndef ALP_CHIPS_UBLOX_MAX_M10S_H
#define ALP_CHIPS_UBLOX_MAX_M10S_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver context for one MAX-M10S receiver.
 *
 * Caller-allocated and owned; bind it with ublox_max_m10s_init() and release it
 * with ublox_max_m10s_deinit(). Treat fields as read-only. Not thread-safe:
 * serialise calls that share one context (and the underlying UART) externally.
 */
typedef struct {
	alp_uart_t *port;        /**< Borrowed UART handle; not closed by deinit(). */
	bool        initialised; /**< True once init() succeeds; gates read/deinit. */
} ublox_max_m10s_t;

/**
 * @brief Bind the context to a caller-opened UART port.
 *
 * Does not configure the UART or the receiver; the caller must open the port at
 * the module's baud (NMEA default 9600).
 *
 * @param dev   Caller-allocated context to populate. Must be non-NULL.
 * @param port  Open, caller-owned UART handle; borrowed for the device lifetime
 *              (not released by ublox_max_m10s_deinit()). Must be non-NULL.
 * @return @ref ALP_OK on success; @ref ALP_ERR_INVAL on a NULL argument.
 */
alp_status_t ublox_max_m10s_init(ublox_max_m10s_t *dev, alp_uart_t *port);

/**
 * @brief Read one NMEA line, byte-by-byte, up to and including the trailing LF.
 *
 * Identical contract to ublox_neo_m9n_read_nmea_line(): reads single bytes until
 * an LF ('\n') or the buffer is full, then writes a terminating NUL. Sentence
 * parsing is left to the caller.
 *
 * @param dev          Initialised context.
 * @param line_buf     Caller-supplied buffer; receives the line plus a NUL terminator.
 * @param line_max     Buffer capacity in bytes including the NUL; must be >= 2.
 * @param len_out      Output: bytes written excluding the NUL (LF included if read).
 *                     Optional; pass NULL to ignore.
 * @param timeout_ms   Maximum wait per byte (not per line).
 * @return @ref ALP_OK on a complete read; @ref ALP_ERR_NOT_READY if not
 *         initialised; @ref ALP_ERR_INVAL on a NULL buffer or @p line_max < 2;
 *         or the UART error (e.g. @ref ALP_ERR_TIMEOUT) from a per-byte read.
 */
alp_status_t ublox_max_m10s_read_nmea_line(ublox_max_m10s_t *dev,
                                           uint8_t          *line_buf,
                                           size_t            line_max,
                                           size_t           *len_out,
                                           uint32_t          timeout_ms);

/**
 * @brief Release the driver context.
 *
 * Clears the bound port and the initialised flag. Does not close the borrowed
 * UART. Safe to call with @p dev NULL (no-op).
 *
 * @param dev  Context to release; may be NULL.
 */
void ublox_max_m10s_deinit(ublox_max_m10s_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_MAX_M10S_H */
