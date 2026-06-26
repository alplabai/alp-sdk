/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file atgm336h.h
 * @brief AllyStar ATGM336H cost-optimised GPS receiver (UART NMEA).
 *
 * China-domestic cost-sensitive multi-GNSS module.  Same UART NMEA
 * shape as the u-blox GNSS drivers.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: AllyStar ATGM336H v2.3 (Feb 2020).
 */

#ifndef ALP_CHIPS_ATGM336H_H
#define ALP_CHIPS_ATGM336H_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Driver context.  Treat as opaque; populated by @ref atgm336h_init. */
typedef struct {
	alp_uart_t *port;        /**< Caller-owned UART carrying the module's NMEA stream. */
	bool        initialised; /**< True once @ref atgm336h_init has bound @p port. */
} atgm336h_t;

/**
 * @brief Bind a driver context to a caller-opened UART.
 *
 * Does not configure the UART — the caller owns @p port and must open it at
 * the module's line rate (9600 8N1 by default) before calling.  Ownership of
 * @p port stays with the caller; @ref atgm336h_deinit does not close it.
 *
 * @param dev  Context to initialise.  Must be non-NULL.
 * @param port Open UART carrying the module's NMEA output.  Must be non-NULL.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p dev or @p port is NULL.
 */
alp_status_t atgm336h_init(atgm336h_t *dev, alp_uart_t *port);

/**
 * @brief Read one NUL-terminated NMEA sentence from the module.
 *
 * Reads bytes until a '\n' is seen or @p line_buf is full, then NUL-terminates.
 * Shares the contract of `ublox_neo_m9n_read_nmea_line`.  The per-byte UART
 * read uses @p timeout_ms, so a stalled stream surfaces as the underlying
 * UART status (e.g. ALP_ERR_TIMEOUT).
 *
 * @param dev        Initialised context.  Must be non-NULL.
 * @param line_buf   Caller buffer to receive the sentence (NUL-terminated on
 *                   success).  Must be non-NULL.
 * @param line_max   Capacity of @p line_buf in bytes, including the NUL.  Must
 *                   be >= 2.
 * @param len_out    On success, receives the sentence length excluding the
 *                   trailing NUL.  May be NULL if not needed.
 * @param timeout_ms Per-byte UART read timeout, milliseconds.
 * @return ALP_OK on a complete or buffer-bounded line; ALP_ERR_NOT_READY if
 *         @p dev is uninitialised; ALP_ERR_INVAL if @p line_buf is NULL or
 *         @p line_max < 2; otherwise the propagated UART read status.
 */
alp_status_t atgm336h_read_nmea_line(
    atgm336h_t *dev, uint8_t *line_buf, size_t line_max, size_t *len_out, uint32_t timeout_ms);

/**
 * @brief Release the driver context.
 *
 * Clears @p dev's state; does not close the UART (the caller owns it) and does
 * not power down the module.  NULL @p dev is a no-op.
 *
 * @param dev Context to release.  May be NULL.
 */
void atgm336h_deinit(atgm336h_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ATGM336H_H */
