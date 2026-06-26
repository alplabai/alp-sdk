/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ti_ds90ub953_954.h
 * @brief TI DS90UB953-Q1 + DS90UB954-Q1 FPD-Link III camera SerDes.
 *
 * Serialiser (DS90UB953) sits at the remote camera end; the
 * deserialiser (DS90UB954) sits on the host board.  Pair carries
 * MIPI CSI-2 + back-channel I²C + back-channel GPIO over a single
 * coax up to 15 m.  Common in ADAS test rigs + industrial machine
 * vision.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — chip-ID + soft reset only.
 *
 * Datasheet: TI DS90UB953-Q1 SNLS554, DS90UB954-Q1 SNLS555.
 */

#ifndef ALP_CHIPS_TI_DS90UB953_954_H
#define ALP_CHIPS_TI_DS90UB953_954_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I²C addresses (IDX strap-selectable, see datasheet). */
#define DS90UB953_I2C_ADDR_DEFAULT 0x18u
#define DS90UB954_I2C_ADDR_DEFAULT 0x30u

/** DEVICE_ID register offset on the serialiser (read to confirm presence). */
#define DS90UB953_REG_DEVICE_ID 0x00u
/** DEVICE_ID register offset on the deserialiser (read to confirm presence). */
#define DS90UB954_REG_DEVICE_ID 0x00u

/** @brief Driver context for one FPD-Link III SerDes pair. */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed I²C bus the pair sits on; not owned. */
	uint8_t    ser_addr;    /**< Serialiser (DS90UB953) 7-bit addr. */
	uint8_t    des_addr;    /**< Deserialiser (DS90UB954) 7-bit addr. */
	bool       initialised; /**< True once ti_ds90ub_init() probed both ends. */
} ti_ds90ub_t;

/**
 * @brief Probe both sides of the FPD-Link III pair over I²C.
 *
 * Reads the DEVICE_ID at each address.  Both must respond for the
 * link to be considered up.
 *
 * @param dev       Caller-allocated context; populated on success.
 * @param bus       Caller-opened I²C bus; borrowed, must outlive @p dev.
 * @param ser_addr  7-bit serialiser address (must be non-zero).
 * @param des_addr  7-bit deserialiser address (must be non-zero).
 * @return ALP_OK; ALP_ERR_INVAL if @p dev / @p bus is NULL or an addr is 0;
 *         ALP_ERR_IO if either end's DEVICE_ID reads back 0 / 0xFF (no link);
 *         the underlying I²C error otherwise.
 */
alp_status_t ti_ds90ub_init(ti_ds90ub_t *dev, alp_i2c_t *bus, uint8_t ser_addr, uint8_t des_addr);

/**
 * @brief Read DEVICE_ID from the deserialiser (host-side) end.
 *
 * @param dev     Initialised context.
 * @param id_out  Receives the DEVICE_ID byte.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; ALP_ERR_INVAL if
 *         @p id_out is NULL; the underlying I²C error otherwise.
 */
alp_status_t ti_ds90ub_read_des_id(ti_ds90ub_t *dev, uint8_t *id_out);

/**
 * @brief Read DEVICE_ID from the serialiser (camera-side) end.
 *
 * @param dev     Initialised context.
 * @param id_out  Receives the DEVICE_ID byte.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; ALP_ERR_INVAL if
 *         @p id_out is NULL; the underlying I²C error otherwise.
 */
alp_status_t ti_ds90ub_read_ser_id(ti_ds90ub_t *dev, uint8_t *id_out);

/**
 * @brief Issue soft reset on both ends (RESET_CTL bit 0).
 *
 * @param dev  Initialised context.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I²C
 *         error otherwise.
 */
alp_status_t ti_ds90ub_soft_reset(ti_ds90ub_t *dev);

/**
 * @brief Release the driver context (clears @c initialised).
 *
 * Does not touch the borrowed I²C bus.  NULL tolerated.
 *
 * @param dev  Context to release, or NULL.
 */
void ti_ds90ub_deinit(ti_ds90ub_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TI_DS90UB953_954_H */
