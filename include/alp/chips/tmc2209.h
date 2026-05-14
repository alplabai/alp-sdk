/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tmc2209.h
 * @brief Trinamic TMC2209 silent stepper driver (UART-controlled).
 *
 * Step/dir + UART-tunable bipolar stepper driver dominant in the
 * desktop-3D-printer ecosystem.  This driver covers the UART
 * register-access surface; step/dir pacing reuses the
 * `<alp/pwm.h>` portable layer the way `drv8825` already does.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Driver status: [stub-impl] — UART CRC + register-read /
 *   register-write helpers.  GCONF / IHOLD_IRUN / CHOPCONF setters
 *   land in follow-up commits.
 *
 * Datasheet: Trinamic TMC2209 v1.09 (Aug 2020).
 */

#ifndef ALP_CHIPS_TMC2209_H
#define ALP_CHIPS_TMC2209_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    alp_uart_t *port;
    uint8_t     slave_addr; /**< 0..3 (MS1 + MS2 strap-selected). */
    bool        initialised;
} tmc2209_t;

/** @brief Bind context to caller-opened UART port. */
alp_status_t tmc2209_init(tmc2209_t *dev, alp_uart_t *port, uint8_t slave_addr);

/** @brief Read a TMC2209 register (32-bit). */
alp_status_t tmc2209_read_reg(tmc2209_t *dev, uint8_t reg, uint32_t *val_out);

/** @brief Write a TMC2209 register (32-bit). */
alp_status_t tmc2209_write_reg(tmc2209_t *dev, uint8_t reg, uint32_t val);

/** @brief Release driver context. */
void tmc2209_deinit(tmc2209_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TMC2209_H */
