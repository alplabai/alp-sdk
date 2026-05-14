/*
 * Copyright 2026 ALP Lab AB
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

typedef struct {
    alp_uart_t *port;
    bool        initialised;
} atgm336h_t;

/** @brief Bind context to caller-opened UART. */
alp_status_t atgm336h_init(atgm336h_t *dev, alp_uart_t *port);

/** @brief Read one NMEA line.  See `ublox_neo_m9n_read_nmea_line`. */
alp_status_t atgm336h_read_nmea_line(atgm336h_t *dev, uint8_t *line_buf, size_t line_max,
                                     size_t *len_out, uint32_t timeout_ms);

/** @brief Release driver context. */
void atgm336h_deinit(atgm336h_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ATGM336H_H */
