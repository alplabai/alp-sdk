/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hx711.h
 * @brief Avia Semi HX711 24-bit ADC for load cells (bit-banged 2-wire).
 *
 * Classic load-cell front-end.  The protocol is a custom 2-wire
 * synchronous serial (PD_SCK clock + DOUT data), bit-banged from
 * host GPIOs — NOT real SPI.  This driver wraps the bit-banged
 * read using the portable `<alp/gpio.h>` surface.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — read API present; DOUT-pin
 *   ready-wait is timing-only at v0.5 (no IRQ wake); fully fleshed
 *   out alongside the v0.6 cooperative timer surface.
 *
 * Datasheet: Avia Semi HX711 v2.1 (Apr 2017).
 */

#ifndef ALP_CHIPS_HX711_H
#define ALP_CHIPS_HX711_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Gain selection -- determined by number of trailing clock pulses
 *  after the 24 data bits. */
typedef enum {
    HX711_GAIN_128 = 25, /**< Channel A, gain 128 (25 pulses total) */
    HX711_GAIN_32  = 26, /**< Channel B, gain 32  (26 pulses total) */
    HX711_GAIN_64  = 27, /**< Channel A, gain 64  (27 pulses total) */
} hx711_gain_t;

typedef struct {
    alp_gpio_t  *sck;
    alp_gpio_t  *dout;
    hx711_gain_t gain;
    bool         initialised;
} hx711_t;

/** @brief Bind context to caller-opened SCK + DOUT GPIOs. */
alp_status_t hx711_init(hx711_t *dev, alp_gpio_t *sck, alp_gpio_t *dout, hx711_gain_t gain);

/** @brief Block until DOUT goes low (chip ready) or timeout fires. */
alp_status_t hx711_wait_ready(hx711_t *dev, uint32_t timeout_ms);

/**
 * @brief Read one signed 24-bit sample (sign-extended to int32).
 *
 * Caller is responsible for calling @ref hx711_wait_ready first.
 */
alp_status_t hx711_read_raw(hx711_t *dev, int32_t *value_out);

/** @brief Release driver context. */
void hx711_deinit(hx711_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_HX711_H */
