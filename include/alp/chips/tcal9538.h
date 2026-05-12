/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tcal9538.h
 * @brief TI TCA9538 / TCAL9538 8-channel I2C I/O expander driver.
 *
 * Both parts share the same register layout; TCAL adds latched-
 * interrupt regs (0x40+) that this driver doesn't yet surface
 * (v0.3.x).  Eight I/O pins, individually configurable as
 * inputs or outputs, with optional polarity inversion.  7-bit
 * address strap A1A0 selects 0x70..0x73 (TCA9538) or
 * 0x70..0x73 (TCAL9538 same range).
 *
 * On the E1M EVK the chip sits on E1M_I2C0 at 0x72 (A1=1, A0=0)
 * and fans out LCD / camera / capacitive-touch control lines plus
 * four sensor interrupt inputs.  See
 * `<alp/boards/alp_e1m_evk.h>`'s `evk_ioexp_pin_t` enum
 * for the EVK-side pin layout.
 *
 * Register map per TI TCA9538 datasheet (SCPS220):
 *   0x00  Input port      (RO)  Reflects external pin levels.
 *   0x01  Output port     (RW)  Drives output pins (where
 *                                Configuration bit = 0).
 *   0x02  Polarity        (RW)  XOR mask for the Input register.
 *   0x03  Configuration   (RW)  1 = input, 0 = output.  Default:
 *                                all inputs (0xFF).
 */

#ifndef ALP_CHIPS_TCAL9538_H
#define ALP_CHIPS_TCAL9538_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCAL9538_I2C_ADDR_BASE 0x70u /**< A1=0, A0=0. */

typedef enum {
    TCAL9538_DIR_OUTPUT = 0, /**< Configuration bit = 0. */
    TCAL9538_DIR_INPUT  = 1, /**< Configuration bit = 1. */
} tcal9538_direction_t;

typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr;
    /* Cached register state -- keeps the driver from a
     * read-modify-write cycle on every set_pin / set_direction call.
     * Synced with the chip on init via a register read-back. */
    uint8_t cfg_cache; /**< Configuration reg (0x03). */
    uint8_t out_cache; /**< Output port reg (0x01). */
} tcal9538_t;

/**
 * @brief Probe + cache the chip's register state.
 *
 * @param[in] addr_7bit  7-bit I2C address (0x70..0x73 by strap).
 *                       Use 0 to fall back to TCAL9538_I2C_ADDR_BASE.
 */
alp_status_t tcal9538_init(tcal9538_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Set the direction (input/output) of a single pin.
 * @param pin  0..7
 */
alp_status_t tcal9538_set_direction(tcal9538_t *ctx, uint8_t pin, tcal9538_direction_t dir);

/**
 * @brief Set directions of multiple pins via mask + value.
 * Bit N of @p mask = 1 means "apply"; bit N of @p value = 1 means
 * "input", 0 means "output".  This is the bulk variant of
 * `tcal9538_set_direction` and avoids 8 round-trips for typical
 * "configure 4 outputs at once" calls.
 */
alp_status_t tcal9538_set_directions(tcal9538_t *ctx, uint8_t mask, uint8_t value);

/** @brief Drive a single output pin to @p level. */
alp_status_t tcal9538_set(tcal9538_t *ctx, uint8_t pin, bool level);

/** @brief Read a single input pin's current level. */
alp_status_t tcal9538_get(tcal9538_t *ctx, uint8_t pin, bool *level_out);

/** @brief Read all 8 input pins as a bitmap. */
alp_status_t tcal9538_read_all(tcal9538_t *ctx, uint8_t *port_out);

/** @brief Write all 8 output pins at once.  Bits with direction =
 *  input are ignored by the chip. */
alp_status_t tcal9538_write_all(tcal9538_t *ctx, uint8_t port);

/** @brief Release the driver context.  Idempotent. */
void         tcal9538_deinit(tcal9538_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TCAL9538_H */
