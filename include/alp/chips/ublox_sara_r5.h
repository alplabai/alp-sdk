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

typedef struct {
    alp_uart_t *port;
    alp_gpio_t *pwr_on;
    alp_gpio_t *reset;
    bool        initialised;
} ublox_sara_r5_t;

/** @brief Bind context to caller-opened UART + GPIOs. */
alp_status_t ublox_sara_r5_init(ublox_sara_r5_t *dev, alp_uart_t *port, alp_gpio_t *pwr_on,
                                alp_gpio_t *reset);

/** @brief Pulse PWR_ON for 1500 ms per u-blox HW integration guide. */
alp_status_t ublox_sara_r5_power_on(ublox_sara_r5_t *dev);

/** @brief Send a single AT command. */
alp_status_t ublox_sara_r5_send_cmd(ublox_sara_r5_t *dev, const char *at_cmd);

/** @brief Read up to @p max bytes of UART response. */
alp_status_t ublox_sara_r5_read_response(ublox_sara_r5_t *dev, uint8_t *buf, size_t max,
                                         size_t *received_out, uint32_t timeout_ms);

/** @brief Release driver context. */
void ublox_sara_r5_deinit(ublox_sara_r5_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_UBLOX_SARA_R5_H */
