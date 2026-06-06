/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file quectel_bg77.h
 * @brief Quectel BG77 newer LTE-M / NB-IoT cellular module (UART AT).
 *
 * Newer-generation Quectel cellular module; same AT-command shell
 * as `quectel_bg95`, smaller power envelope, plus integrated GNSS
 * receiver (the GNSS surface lives behind dedicated AT commands).
 * Driver shape is identical to BG95.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_QUECTEL_BG77_H
#define ALP_CHIPS_QUECTEL_BG77_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    alp_uart_t *port;
    alp_gpio_t *pwrkey;
    alp_gpio_t *reset;
    bool        initialised;
} quectel_bg77_t;

/** @brief Bind context to caller-opened UART + GPIOs. */
alp_status_t quectel_bg77_init(quectel_bg77_t *dev, alp_uart_t *port, alp_gpio_t *pwrkey,
                               alp_gpio_t *reset);

/** @brief Pulse PWRKEY for 500 ms.  Caller waits ~5 s after. */
alp_status_t quectel_bg77_power_on(quectel_bg77_t *dev);

/** @brief Send a single AT command. */
alp_status_t quectel_bg77_send_cmd(quectel_bg77_t *dev, const char *at_cmd);

/** @brief Read up to @p max bytes of UART response. */
alp_status_t quectel_bg77_read_response(quectel_bg77_t *dev, uint8_t *buf, size_t max,
                                        size_t *received_out, uint32_t timeout_ms);

/** @brief Release driver context. */
void quectel_bg77_deinit(quectel_bg77_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_QUECTEL_BG77_H */
