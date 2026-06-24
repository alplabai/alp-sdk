/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vl53l1x.h
 * @brief STMicroelectronics VL53L1X time-of-flight ranger (I²C).
 *
 * 940 nm laser ToF sensor, up to 4 m range, ±2.5 % accuracy.
 * Driver covers chip-ID probe + soft reset only; the full ranging
 * sequence (sysrange-start, polling, range read) lands when the
 * ST ULD library import lands.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 *
 * Datasheet: ST VL53L1X DS13088 Rev 7 (Jul 2021).
 */

#ifndef ALP_CHIPS_VL53L1X_H
#define ALP_CHIPS_VL53L1X_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VL53L1X_I2C_ADDR_DEFAULT 0x29u

#define VL53L1X_REG_IDENTIFICATION_MODEL_ID 0x010Fu
#define VL53L1X_MODEL_ID                    0xEAu

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} vl53l1x_t;

/** @brief Bind context and verify MODEL_ID. */
alp_status_t vl53l1x_init(vl53l1x_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read MODEL_ID for liveness. */
alp_status_t vl53l1x_read_id(vl53l1x_t *dev, uint8_t *id_out);

/** @brief Issue a software reset (SOFT_RESET register). */
alp_status_t vl53l1x_soft_reset(vl53l1x_t *dev);

/** @brief Release driver context. */
void vl53l1x_deinit(vl53l1x_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VL53L1X_H */
