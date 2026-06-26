/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maxim_max9295_9296.h
 * @brief ADI (Maxim) MAX9295A + MAX9296A GMSL2 camera SerDes.
 *
 * 6 Gbps Gigabit Multimedia Serial Link 2 over coax.  Same overall
 * shape as TI's FPD-Link III pair (`ti_ds90ub953_954`) but at
 * 6 Gbps with native automotive-grade qualification.  Dominant
 * SerDes in ADAS production today.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — chip-ID + soft reset only.
 *
 * Datasheets: MAX9295A (Rev 5), MAX9296A (Rev 5).
 */

#ifndef ALP_CHIPS_MAXIM_MAX9295_9296_H
#define ALP_CHIPS_MAXIM_MAX9295_9296_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX9295_I2C_ADDR_DEFAULT 0x40u /**< Serialiser. */
#define MAX9296_I2C_ADDR_DEFAULT 0x48u /**< Deserialiser. */

#define MAX929x_REG_DEV_ID  0x000Du /**< Device-ID register, common to both ends. */
#define MAX929x_REG_DEV_REV 0x000Eu /**< Silicon-revision register. */

#define MAX9295_DEV_ID 0x91u /**< Expected DEV_ID of the MAX9295A serialiser. */
#define MAX9296_DEV_ID 0x94u /**< Expected DEV_ID of the MAX9296A deserialiser. */

/** @brief GMSL2 SerDes-pair driver context. */
typedef struct {
	alp_i2c_t *bus;         /**< Open I²C bus shared by both ends (not owned). */
	uint8_t    ser_addr;    /**< 7-bit MAX9295A serialiser address. */
	uint8_t    des_addr;    /**< 7-bit MAX9296A deserialiser address. */
	bool       initialised; /**< True once both ends were probed by init. */
} maxim_gmsl2_t;

/**
 * @brief Probe both ends of the GMSL2 pair over I²C.
 *
 * Verifies MAX9295A serialiser DEV_ID == 0x91 and MAX9296A
 * deserialiser DEV_ID == 0x94.
 *
 * @param dev       Driver context to populate (output).
 * @param bus       Open I²C bus (not owned; must outlive @p dev).
 * @param ser_addr  7-bit serialiser address (typically MAX9295_I2C_ADDR_DEFAULT).
 * @param des_addr  7-bit deserialiser address (typically MAX9296_I2C_ADDR_DEFAULT).
 * @return ALP_OK on success; an ALP_ERR_* status if either DEV_ID mismatches or I²C fails.
 */
alp_status_t
maxim_gmsl2_init(maxim_gmsl2_t *dev, alp_i2c_t *bus, uint8_t ser_addr, uint8_t des_addr);

/**
 * @brief Read DEV_ID from the deserialiser.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Receives the raw DEV_ID byte (expected MAX9296_DEV_ID).
 * @return ALP_OK on success; an ALP_ERR_* status on I²C failure.
 */
alp_status_t maxim_gmsl2_read_des_id(maxim_gmsl2_t *dev, uint8_t *id_out);

/**
 * @brief Read DEV_ID from the serialiser.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Receives the raw DEV_ID byte (expected MAX9295_DEV_ID).
 * @return ALP_OK on success; an ALP_ERR_* status on I²C failure.
 */
alp_status_t maxim_gmsl2_read_ser_id(maxim_gmsl2_t *dev, uint8_t *id_out);

/**
 * @brief Issue soft reset on both ends (CTRL0 bit 6).
 *
 * @param dev  Initialised driver context.
 * @return ALP_OK on success; an ALP_ERR_* status on I²C failure.
 */
alp_status_t maxim_gmsl2_soft_reset(maxim_gmsl2_t *dev);

/**
 * @brief Release driver context.
 *
 * @param dev  Driver context, or NULL (tolerated; no-op).
 */
void maxim_gmsl2_deinit(maxim_gmsl2_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAXIM_MAX9295_9296_H */
