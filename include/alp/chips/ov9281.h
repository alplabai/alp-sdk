/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ov9281.h
 * @brief OmniVision OV9281 1 MP global-shutter mono MIPI CSI-2 sensor.
 *
 * Global-shutter monochrome 1280 × 800 sensor used for AR/VR
 * tracking, ALPR, and high-speed industrial inspection.  SCCB
 * config side only; pixel data flows over MIPI CSI-2 to the SoC
 * camera receiver behind `<alp/camera.h>`.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — chip-ID + soft reset only.
 *
 * Datasheet: OmniVision OV9281 (Rev 1.06, Oct 2017).
 */

#ifndef ALP_CHIPS_OV9281_H
#define ALP_CHIPS_OV9281_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OV9281_I2C_ADDR_LOW  0x60u /**< AGND on SID strap. */
#define OV9281_I2C_ADDR_HIGH 0x62u /**< AVDD on SID strap. */

#define OV9281_REG_CHIP_ID_HI 0x300Au
#define OV9281_REG_CHIP_ID_LO 0x300Bu
#define OV9281_CHIP_ID        0x9281u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} ov9281_t;

/** @brief Bind context and verify chip ID. */
alp_status_t ov9281_init(ov9281_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read CHIP_ID for liveness checks. */
alp_status_t ov9281_read_id(ov9281_t *dev, uint16_t *id_out);

/** @brief Issue a software reset (system-reset bit at 0x0103). */
alp_status_t ov9281_soft_reset(ov9281_t *dev);

/** @brief Release driver context.  Idempotent.  NULL tolerated. */
void ov9281_deinit(ov9281_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OV9281_H */
