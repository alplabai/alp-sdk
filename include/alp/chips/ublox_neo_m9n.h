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

typedef struct {
	alp_uart_t *port;
	bool        initialised;
} ublox_neo_m9n_t;

/** @brief Bind context to caller-opened UART port. */
alp_status_t ublox_neo_m9n_init(ublox_neo_m9n_t *dev, alp_uart_t *port);

/**
 * @brief Read one NMEA line (terminated by LF).
 *
 * @param dev          Initialised context.
 * @param line_buf     Caller-supplied buffer.
 * @param line_max     Max bytes (incl. terminator).
 * @param len_out      Output: total bytes written (excl. NUL).  Optional.
 * @param timeout_ms   Max wait per byte.
 *
 * @return `ALP_OK` on full line read.
 */
alp_status_t ublox_neo_m9n_read_nmea_line(ublox_neo_m9n_t *dev,
                                          uint8_t         *line_buf,
                                          size_t           line_max,
                                          size_t          *len_out,
                                          uint32_t         timeout_ms);

/** @brief Release driver context. */
void ublox_neo_m9n_deinit(ublox_neo_m9n_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_NEO_M9N_H */
