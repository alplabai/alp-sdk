/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tlv320aic3204.h
 * @brief TI TLV320AIC3204 premium stereo audio codec (I²C + I²S).
 *
 * Premium TI stereo codec with on-chip miniDSP.  This driver covers
 * the I²C control surface only -- register R/W + page selection.
 * Audio sample flow over I²S is delegated to `<alp/i2s.h>`.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: TI TLV320AIC3204 SLAS549G (Nov 2018).
 */

#ifndef ALP_CHIPS_TLV320AIC3204_H
#define ALP_CHIPS_TLV320AIC3204_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TLV320AIC3204_I2C_ADDR_LOW 0x18u
#define TLV320AIC3204_I2C_ADDR_HIGH 0x19u

#define TLV320AIC3204_REG_PAGE_SELECT 0x00u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	uint8_t    current_page;
	bool       initialised;
} tlv320aic3204_t;

/** @brief Bind context to caller-opened I²C bus. */
alp_status_t tlv320aic3204_init(tlv320aic3204_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Latch the active page (0..255). */
alp_status_t tlv320aic3204_select_page(tlv320aic3204_t *dev, uint8_t page);

/** @brief Read register at the current page. */
alp_status_t tlv320aic3204_read_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t *val);

/** @brief Write register at the current page. */
alp_status_t tlv320aic3204_write_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t val);

/** @brief Issue software reset (page 0, reg 1, write 1). */
alp_status_t tlv320aic3204_soft_reset(tlv320aic3204_t *dev);

/** @brief Release driver context. */
void tlv320aic3204_deinit(tlv320aic3204_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TLV320AIC3204_H */
