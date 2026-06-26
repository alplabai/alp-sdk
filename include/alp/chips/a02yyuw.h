/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file a02yyuw.h
 * @brief DFRobot SEN0312 / A02YYUW waterproof ultrasonic ranger (UART).
 *
 * Cost-optimised IP67 ultrasonic distance sensor.  Streams one
 * 4-byte distance frame per ~100 ms over UART (TTL 9600/8N1):
 *
 *   `[0xFF][HI][LO][CHK]`  where `distance_mm = (HI << 8) | LO`,
 *   `CHK = (0xFF + HI + LO) & 0xFF`.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: DFRobot A02YYUW v2.0 (2022).
 */

#ifndef ALP_CHIPS_A02YYUW_H
#define ALP_CHIPS_A02YYUW_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A02YYUW_BAUD_RATE 9600u /**< Fixed UART baud rate, bit/s (8N1). */
#define A02YYUW_FRAME_LEN 4u    /**< Bytes per distance frame: [0xFF][HI][LO][CHK]. */

/** @brief Driver context.  Zero-initialise, then pass to @ref a02yyuw_init. */
typedef struct {
	alp_uart_t *port;        /**< Bound UART port (not owned; caller keeps it open). */
	bool        initialised; /**< true once @ref a02yyuw_init has bound a port. */
} a02yyuw_t;

/**
 * @brief Bind context to an open UART port (caller configures 9600/8N1).
 *
 * Does not own @p port -- the caller opens it beforehand and closes it
 * after @ref a02yyuw_deinit.
 *
 * @param dev   Caller-allocated context to initialise.  Must be non-NULL.
 * @param port  Open UART configured for 9600/8N1.  Must be non-NULL.
 * @return `ALP_OK` on success, `ALP_ERR_INVAL` if @p dev or @p port is NULL.
 */
alp_status_t a02yyuw_init(a02yyuw_t *dev, alp_uart_t *port);

/**
 * @brief Read one distance frame.  Blocks up to @p timeout_ms.
 *
 * @param dev          Initialised context.
 * @param distance_mm  Output: distance in millimetres.
 * @param timeout_ms   Max wait per byte.
 * @return `ALP_OK` on success, `ALP_ERR_IO` on checksum mismatch,
 *         `ALP_ERR_TIMEOUT` on UART timeout.
 */
alp_status_t a02yyuw_read_distance(a02yyuw_t *dev, uint16_t *distance_mm, uint32_t timeout_ms);

/**
 * @brief Release driver context.  Does not close the bound UART port.
 *
 * @param dev  Context from @ref a02yyuw_init, or NULL (no-op).
 */
void a02yyuw_deinit(a02yyuw_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_A02YYUW_H */
