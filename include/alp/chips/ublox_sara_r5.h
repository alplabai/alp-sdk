/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ublox_sara_r5.h
 * @brief u-blox SARA-R5 LTE-M cellular module (UART AT).
 *
 * Board-certified LTE Cat M1 module.  Same AT-command shell
 * shape as `quectel_bg95` -- different power-rail sequencing
 * (PWR_ON pulse + RESET line).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_UBLOX_SARA_R5_H
#define ALP_CHIPS_UBLOX_SARA_R5_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver context for one SARA-R5 module.
 *
 * Caller-allocated and owned; bind it with ublox_sara_r5_init() and release it
 * with ublox_sara_r5_deinit(). Treat fields as read-only. Not thread-safe:
 * serialise calls that share one context (and the underlying UART) externally.
 */
typedef struct {
	alp_uart_t *port;        /**< Borrowed UART handle for the AT shell; not closed by deinit(). */
	alp_gpio_t *pwr_on;      /**< Optional PWR_ON line; NULL if not wired (power_on() rejected). */
	alp_gpio_t *reset;       /**< Optional RESET_N line; NULL if not wired. Borrowed. */
	bool        initialised; /**< True once init() succeeds; gates all other calls. */
} ublox_sara_r5_t;

/**
 * @brief Bind the context to a caller-opened UART and optional control GPIOs.
 *
 * Borrows all handles for the device lifetime; none are released by
 * ublox_sara_r5_deinit(). The GPIOs are optional and may be NULL.
 *
 * @param dev     Caller-allocated context to populate. Must be non-NULL.
 * @param port    Open, caller-owned UART handle for the AT shell. Must be non-NULL.
 * @param pwr_on  PWR_ON control line, or NULL if not wired.
 * @param reset   RESET_N control line, or NULL if not wired.
 * @return @ref ALP_OK on success; @ref ALP_ERR_INVAL if @p dev or @p port is NULL.
 */
alp_status_t
ublox_sara_r5_init(ublox_sara_r5_t *dev, alp_uart_t *port, alp_gpio_t *pwr_on, alp_gpio_t *reset);

/**
 * @brief Drive a power-on pulse on PWR_ON.
 *
 * Asserts PWR_ON, busy-waits 1500 ms (per the u-blox SARA-R5 hardware
 * integration guide), then deasserts. Blocks for the full pulse.
 *
 * @param dev  Initialised context whose @c pwr_on line is wired.
 * @return @ref ALP_OK on success; @ref ALP_ERR_NOT_READY if not initialised;
 *         @ref ALP_ERR_NOSUPPORT if no PWR_ON line was supplied; or the GPIO
 *         write error.
 */
alp_status_t ublox_sara_r5_power_on(ublox_sara_r5_t *dev);

/**
 * @brief Send one AT command over the UART.
 *
 * Writes @p at_cmd verbatim, then appends a CR+LF terminator unless the string
 * already ends in '\n'. Does not wait for or parse the response.
 *
 * @param dev     Initialised context.
 * @param at_cmd  NUL-terminated AT command (without or with its own line ending).
 *                Must be non-NULL.
 * @return @ref ALP_OK on success; @ref ALP_ERR_NOT_READY if not initialised;
 *         @ref ALP_ERR_INVAL if @p at_cmd is NULL; or the UART write error.
 */
alp_status_t ublox_sara_r5_send_cmd(ublox_sara_r5_t *dev, const char *at_cmd);

/**
 * @brief Read raw response bytes from the UART.
 *
 * @note [stub-impl] Performs a single blocking UART read of up to @p max bytes
 *       and, on success, reports @p max via @p received_out rather than the true
 *       count. Do not rely on @p received_out until the real implementation lands.
 *
 * @param dev           Initialised context.
 * @param buf           Caller-supplied buffer. Must be non-NULL.
 * @param max           Buffer capacity in bytes; must be non-zero.
 * @param received_out  Output: byte count (see stub note above). Optional; may be NULL.
 * @param timeout_ms    Maximum wait for the read.
 * @return @ref ALP_OK on success; @ref ALP_ERR_NOT_READY if not initialised;
 *         @ref ALP_ERR_INVAL on a NULL buffer or zero @p max; or the UART error
 *         (e.g. @ref ALP_ERR_TIMEOUT).
 */
alp_status_t ublox_sara_r5_read_response(
    ublox_sara_r5_t *dev, uint8_t *buf, size_t max, size_t *received_out, uint32_t timeout_ms);

/**
 * @brief Release the driver context.
 *
 * Clears the bound UART and GPIO handles and the initialised flag. Does not close
 * the borrowed handles or power down the module. Safe to call with @p dev NULL.
 *
 * @param dev  Context to release; may be NULL.
 */
void ublox_sara_r5_deinit(ublox_sara_r5_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_SARA_R5_H */
