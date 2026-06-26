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

/** 7-bit I²C address with the ADDR strap tied low (default). */
#define TLV320AIC3204_I2C_ADDR_LOW 0x18u
/** 7-bit I²C address with the ADDR strap tied high. */
#define TLV320AIC3204_I2C_ADDR_HIGH 0x19u

/** Page-select register; reg 0 of every page selects the active page. */
#define TLV320AIC3204_REG_PAGE_SELECT 0x00u

/** @brief Driver context for the I²C control surface of one codec. */
typedef struct {
	alp_i2c_t *bus;          /**< Borrowed I²C bus; not owned. */
	uint8_t    addr;         /**< 7-bit I²C slave address. */
	uint8_t    current_page; /**< Last page latched via _select_page(). */
	bool       initialised;  /**< True once tlv320aic3204_init() ran. */
} tlv320aic3204_t;

/**
 * @brief Bind context to a caller-opened I²C bus.
 *
 * Does not touch the chip; the first register access does.
 *
 * @param dev       Caller-allocated context; populated on success.
 * @param bus       Caller-opened I²C bus; borrowed, must outlive @p dev.
 * @param i2c_addr  7-bit slave address (must be non-zero).
 * @return ALP_OK; ALP_ERR_INVAL if @p dev / @p bus is NULL or @p i2c_addr is 0.
 */
alp_status_t tlv320aic3204_init(tlv320aic3204_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Latch the active register page (0..255).
 *
 * No-op (returns ALP_OK) if @p page already selected.
 *
 * @param dev   Initialised context.
 * @param page  Page number to make active.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I²C
 *         error otherwise.
 */
alp_status_t tlv320aic3204_select_page(tlv320aic3204_t *dev, uint8_t page);

/**
 * @brief Read a register at the currently-selected page.
 *
 * @param dev  Initialised context.
 * @param reg  Register offset within the active page.
 * @param val  Receives the register byte.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; ALP_ERR_INVAL if
 *         @p val is NULL; the underlying I²C error otherwise.
 */
alp_status_t tlv320aic3204_read_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t *val);

/**
 * @brief Write a register at the currently-selected page.
 *
 * @param dev  Initialised context.
 * @param reg  Register offset within the active page.
 * @param val  Byte to write.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I²C
 *         error otherwise.
 */
alp_status_t tlv320aic3204_write_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t val);

/**
 * @brief Issue a software reset (page 0, reg 1, write 1).
 *
 * Selects page 0 first, so it also resets @c current_page tracking.
 *
 * @param dev  Initialised context.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I²C
 *         error otherwise.
 */
alp_status_t tlv320aic3204_soft_reset(tlv320aic3204_t *dev);

/**
 * @brief Release the driver context (clears @c initialised).
 *
 * Does not touch the borrowed I²C bus.  NULL tolerated.
 *
 * @param dev  Context to release, or NULL.
 */
void tlv320aic3204_deinit(tlv320aic3204_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TLV320AIC3204_H */
