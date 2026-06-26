/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tas2563.h
 * @brief Texas Instruments TAS2563 smart Class-D mono speaker amp.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Digital input I2S + I2C control + algorithm-driven smart-amp
 * features (DRC, DSP, EQ, IV-sense feedback for excursion +
 * thermal protection).  This driver is intentionally thin --
 * smart-amp tuning runs in TI's PPC3 (PurePath Console) host
 * tool, which generates a binary "tuning blob" that the host
 * MCU streams into the chip via I2C.
 *
 * v0.3 driver scope:
 *   - I2C connectivity probe (read CHIP_ID).
 *   - Software shutdown / mute / unmute via the MODE_CTRL
 *     register.
 *   - Hardware enable via SD_N (an external GPIO; the caller
 *     supplies an alp_gpio_t handle bound to the EVK's
 *     `EVK_PIN_AMP_ENABLE`).
 *
 * v0.3.x adds:
 *   - tuning-blob loader (PPC3 export -> register bursts)
 *   - I2S configuration via a paired alp_i2s_t (the host's
 *     I2S TX feeds the amp's SDIN; the amp's SDOUT feeds back
 *     IV-sense data on the host's I2S RX)
 *   - fault-pin handling (open-drain IRQ_N on the EVK's
 *     `EVK_PIN_AMP_FAULT` -- routed through host SoC GPIO with
 *     internal pull-up).
 *
 * I2C addresses (TAS2563 Table 7-3):
 *   AD0/SPICLK = GND       -> 0x4C
 *   AD0/SPICLK = 10k to GND -> 0x4D
 *   AD0/SPICLK = 10k to VDD -> 0x4E
 *   AD0/SPICLK = VDD       -> 0x4F
 *   Global broadcast        -> 0x48
 */

#ifndef ALP_CHIPS_TAS2563_H
#define ALP_CHIPS_TAS2563_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAS2563_I2C_ADDR_GND_DIRECT 0x4Cu
#define TAS2563_I2C_ADDR_GND_PULL   0x4Du
#define TAS2563_I2C_ADDR_VDD_PULL   0x4Eu
#define TAS2563_I2C_ADDR_VDD_DIRECT 0x4Fu
#define TAS2563_I2C_ADDR_BROADCAST  0x48u

/** Operating-mode enum mapped onto the chip's MODE_CTRL register. */
typedef enum {
	TAS2563_MODE_ACTIVE   = 0x00, /**< Audio amplification active. */
	TAS2563_MODE_MUTE     = 0x01, /**< PWM muted, register state preserved. */
	TAS2563_MODE_SHUTDOWN = 0x02, /**< Software shutdown, lowest IDD. */
} tas2563_mode_t;

typedef struct {
	bool        initialised; /**< True once tas2563_init() has succeeded. */
	alp_i2c_t  *bus;         /**< I2C control bus the amp sits on (borrowed, not owned). */
	uint8_t     addr;        /**< 7-bit I2C address (one of TAS2563_I2C_ADDR_*). */
	alp_gpio_t *sd_n;        /**< AMP.ENABLE pin (active-high; drive
                              low to assert SD_N hardware shutdown).  NULL if unused. */
} tas2563_t;

/**
 * @brief Probe the chip + (optionally) drive SD_N high to leave
 *        hardware shutdown.
 *
 * @param[out] ctx       Driver context (output; populated on success).
 * @param[in]  bus       Open I2C bus handle the amp sits on.
 * @param[in]  addr_7bit 7-bit I2C address (one of the
 *                       TAS2563_I2C_ADDR_* constants).
 * @param[in]  sd_n      Open GPIO handle bound to AMP.ENABLE.  May
 *                       be NULL if the caller drives SD_N
 *                       elsewhere (or if the pin is tied permanently
 *                       to V+).
 */
alp_status_t tas2563_init(tas2563_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, alp_gpio_t *sd_n);

/**
 * @brief Read the chip's revision register (a no-op-ish sanity check).
 * @param[in]  ctx     Initialised driver context.
 * @param[out] rev_out Receives the REV_ID register value.
 * @return `ALP_OK` on success, or an `alp_status_t` error on bus failure.
 */
alp_status_t tas2563_read_revision(tas2563_t *ctx, uint8_t *rev_out);

/**
 * @brief Switch operating mode via MODE_CTRL register write.
 * @param ctx  Initialised driver context.
 * @param mode One of @ref tas2563_mode_t.
 * @return `ALP_OK` on success, or an `alp_status_t` error on bus failure.
 */
alp_status_t tas2563_set_mode(tas2563_t *ctx, tas2563_mode_t mode);

/**
 * @brief Drive AMP.ENABLE high (resume) or low (hardware shutdown).
 *
 * Bypasses MODE_CTRL -- when SD_N is low, the chip is in HW
 * shutdown regardless of MODE_CTRL.  Useful for fast power-down
 * during a fault or for staging the boot sequence.
 *
 * @param ctx    Initialised driver context (must have been given a non-NULL sd_n).
 * @param enable true drives SD_N high (resume), false drives it low (HW shutdown).
 * @return `ALP_OK` on success, or an `alp_status_t` error (e.g. no SD_N GPIO bound).
 */
alp_status_t tas2563_set_hw_enable(tas2563_t *ctx, bool enable);

/**
 * @brief Release the driver context.  Drops SD_N before returning.
 * @param ctx Driver context to release.
 */
void tas2563_deinit(tas2563_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TAS2563_H */
