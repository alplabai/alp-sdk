/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gc2145.h
 * @brief GalaxyCore GC2145 2 MP DVP camera — config-side driver.
 *
 * China-domestic cost-sensitive 2 MP camera popular in Arduino-
 * Portenta H7's Vision Shield and budget eval boards.  DVP 8-bit
 * parallel pixel egress; SCCB config side only.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_GC2145_H
#define ALP_CHIPS_GC2145_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC2145_I2C_ADDR 0x3Cu

#define GC2145_REG_PID_HI 0xF0u
#define GC2145_REG_PID_LO 0xF1u
#define GC2145_CHIP_ID 0x2145u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} gc2145_t;

/** @brief Bind context and verify chip ID. */
alp_status_t gc2145_init(gc2145_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read CHIP_ID for liveness. */
alp_status_t gc2145_read_id(gc2145_t *dev, uint16_t *id_out);

/** @brief Issue a software reset (0xFE bit 7). */
alp_status_t gc2145_soft_reset(gc2145_t *dev);

/** @brief Release driver context.  NULL tolerated. */
void gc2145_deinit(gc2145_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GC2145_H */
