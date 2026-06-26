/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ublox_neo_m9n.h
 * @brief u-blox NEO-M9N GNSS receiver (UART NMEA).
 *
 * High-precision multi-constellation GNSS module (GPS / GLONASS /
 * Galileo / BeiDou).  Default output is NMEA 0183 at 9600 baud;
 * this driver covers the raw UART byte stream + a thin NMEA-line
 * boundary detector.  Sentence parsing lives in the consuming app.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: u-blox NEO-M9N v1.10 (Sep 2020).
 */

#ifndef ALP_CHIPS_UBLOX_NEO_M9N_H
#define ALP_CHIPS_UBLOX_NEO_M9N_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver context for one NEO-M9N receiver.
 *
 * Caller-allocated and owned; bind it with ublox_neo_m9n_init() and release it
 * with ublox_neo_m9n_deinit(). Treat fields as read-only. Not thread-safe:
 * serialise calls that share one context (and the underlying UART) externally.
 */
typedef struct {
	alp_uart_t *port;        /**< Borrowed UART handle; not closed by deinit(). */
	bool        initialised; /**< True once init() succeeds; gates read/deinit. */
} ublox_neo_m9n_t;

/**
 * @brief Bind the context to a caller-opened UART port.
 *
 * Does not configure the UART or the receiver; the caller must open the port at
 * the module's baud (NMEA default 9600).
 *
 * @param dev   Caller-allocated context to populate. Must be non-NULL.
 * @param port  Open, caller-owned UART handle; borrowed for the device lifetime
 *              (not released by ublox_neo_m9n_deinit()). Must be non-NULL.
 * @return @ref ALP_OK on success; @ref ALP_ERR_INVAL on a NULL argument.
 */
alp_status_t ublox_neo_m9n_init(ublox_neo_m9n_t *dev, alp_uart_t *port);

/**
 * @brief Read one NMEA line, byte-by-byte, up to and including the trailing LF.
 *
 * Reads single bytes until an LF ('\n') is seen or the buffer is full, then writes
 * a terminating NUL. Sentence parsing (the '$...*CC' framing) is left to the caller.
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
alp_status_t ublox_neo_m9n_read_nmea_line(
    ublox_neo_m9n_t *dev, uint8_t *line_buf, size_t line_max, size_t *len_out, uint32_t timeout_ms);

/**
 * @brief Release the driver context.
 *
 * Clears the bound port and the initialised flag. Does not close the borrowed
 * UART. Safe to call with @p dev NULL (no-op).
 *
 * @param dev  Context to release; may be NULL.
 */
void ublox_neo_m9n_deinit(ublox_neo_m9n_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_NEO_M9N_H */
