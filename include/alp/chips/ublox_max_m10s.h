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

typedef struct {
    alp_uart_t *port;
    bool        initialised;
} ublox_max_m10s_t;

/** @brief Bind context to caller-opened UART. */
alp_status_t ublox_max_m10s_init(ublox_max_m10s_t *dev, alp_uart_t *port);

/** @brief Read one NMEA line.  See `ublox_neo_m9n_read_nmea_line`. */
alp_status_t ublox_max_m10s_read_nmea_line(ublox_max_m10s_t *dev, uint8_t *line_buf,
                                           size_t line_max, size_t *len_out, uint32_t timeout_ms);

/** @brief Release driver context. */
void ublox_max_m10s_deinit(ublox_max_m10s_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_MAX_M10S_H */
