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

#define DS90UB953_REG_DEVICE_ID 0x00u
#define DS90UB954_REG_DEVICE_ID 0x00u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    ser_addr; /**< Serialiser (DS90UB953) addr. */
	uint8_t    des_addr; /**< Deserialiser (DS90UB954) addr. */
	bool       initialised;
} ti_ds90ub_t;

/**
 * @brief Probe both sides of the FPD-Link III pair over I²C.
 *
 * Reads the DEVICE_ID at each address.  Both must respond for the
 * link to be considered up.
 */
alp_status_t ti_ds90ub_init(ti_ds90ub_t *dev, alp_i2c_t *bus, uint8_t ser_addr, uint8_t des_addr);

/** @brief Read DEVICE_ID from the deserialiser end. */
alp_status_t ti_ds90ub_read_des_id(ti_ds90ub_t *dev, uint8_t *id_out);

/** @brief Read DEVICE_ID from the serialiser end. */
alp_status_t ti_ds90ub_read_ser_id(ti_ds90ub_t *dev, uint8_t *id_out);

/** @brief Issue soft reset on both ends (RESET_CTL bit 0). */
alp_status_t ti_ds90ub_soft_reset(ti_ds90ub_t *dev);

/** @brief Release driver context.  NULL tolerated. */
void ti_ds90ub_deinit(ti_ds90ub_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TI_DS90UB953_954_H */
