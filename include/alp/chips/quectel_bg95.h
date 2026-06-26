/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file quectel_bg95.h
 * @brief Quectel BG95 LTE-M / NB-IoT cellular module (UART AT).
 *
 * Multi-mode (LTE Cat M1 + Cat NB2 + EGPRS) cellular module
 * controlled over UART using the standard 3GPP AT command set.
 * This driver covers the line-layer AT-command shell only:
 * `init` + `send_cmd` + `read_response` + `deinit`.  The TCP /
 * MQTT / FTP / HTTP profiles built on top of AT live in the
 * `tinygsm` library (Phase 2 §D.lib.iot).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — AT command framing only.
 *
 * Datasheet: Quectel BG95 series HW design Rev 1.2 (Mar 2022).
 */

#ifndef ALP_CHIPS_QUECTEL_BG95_H
#define ALP_CHIPS_QUECTEL_BG95_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Driver context for one BG95 module on a UART + control GPIOs. */
typedef struct {
	alp_uart_t *port;        /**< Caller-opened AT-shell UART (borrowed, not owned). */
	alp_gpio_t *pwrkey;      /**< Active-high PWRKEY pulse line (borrowed). */
	alp_gpio_t *reset;       /**< Optional hardware reset; may be NULL. */
	bool        initialised; /**< True once quectel_bg95_init() has succeeded. */
} quectel_bg95_t;

/**
 * @brief Bind context to caller-opened UART + GPIOs.
 *
 * @param dev    Caller-allocated context to populate.
 * @param port   Open UART handle (borrowed; must outlive @p dev).
 * @param pwrkey Open GPIO for PWRKEY (borrowed).
 * @param reset  Open GPIO for hardware reset, or NULL if unused.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL required argument.
 */
alp_status_t
quectel_bg95_init(quectel_bg95_t *dev, alp_uart_t *port, alp_gpio_t *pwrkey, alp_gpio_t *reset);

/**
 * @brief Pulse PWRKEY for the documented duration (typ. 500 ms).
 *
 * Caller is expected to wait at least 5 s after this returns
 * before sending the first AT command (Quectel boot time).
 *
 * @param dev Initialised driver context.
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL argument.
 */
alp_status_t quectel_bg95_power_on(quectel_bg95_t *dev);

/**
 * @brief Send a single AT command.
 *
 * Caller-supplied CR terminator is OK; the driver appends CRLF if missing.
 *
 * @param dev    Initialised driver context.
 * @param at_cmd NUL-terminated AT command string.
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL argument; a UART
 *         error status on a failed write.
 */
alp_status_t quectel_bg95_send_cmd(quectel_bg95_t *dev, const char *at_cmd);

/**
 * @brief Read up to @p max bytes of response (blocks up to @p timeout_ms).
 *
 * @param dev          Initialised driver context.
 * @param buf          Caller buffer to fill.
 * @param max          Capacity of @p buf in bytes.
 * @param received_out Out-param; receives the byte count read.
 * @param timeout_ms   Maximum time to wait for data, in milliseconds.
 * @return `ALP_OK` on at least one byte received; `ALP_ERR_TIMEOUT`
 *         on no data within timeout; ALP_ERR_INVAL on NULL argument.
 */
alp_status_t quectel_bg95_read_response(
    quectel_bg95_t *dev, uint8_t *buf, size_t max, size_t *received_out, uint32_t timeout_ms);

/**
 * @brief Release driver context.
 *
 * @param dev Driver context; NULL is tolerated as a no-op.
 */
void quectel_bg95_deinit(quectel_bg95_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_QUECTEL_BG95_H */
