/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file max98357a.h
 * @brief ADI (Maxim) MAX98357A 3 W class-D mono amplifier (I²S).
 *
 * Single-pin "shutdown" board-board class-D amp.  No I²C config
 * surface -- gain is set by a single resistor / strap pin.  This
 * driver controls the shutdown GPIO + the I²S handle is owned by
 * the consumer.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: ADI (Maxim) MAX98357A (Rev 5, Mar 2020).
 */

#ifndef ALP_CHIPS_MAX98357A_H
#define ALP_CHIPS_MAX98357A_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    alp_gpio_t *sd_mode; /**< Active-high shutdown / mode-select pin. */
    bool        initialised;
} max98357a_t;

/** @brief Bind context to caller-opened SD_MODE GPIO. */
alp_status_t max98357a_init(max98357a_t *dev, alp_gpio_t *sd_mode);

/** @brief Drive SD_MODE high to enable amp; low to shut down. */
alp_status_t max98357a_set_enabled(max98357a_t *dev, bool enabled);

/** @brief Release driver context. */
void max98357a_deinit(max98357a_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX98357A_H */
