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

#define OV9281_I2C_ADDR_LOW  0x60u /**< 7-bit SCCB address when SID is strapped to AGND. */
#define OV9281_I2C_ADDR_HIGH 0x62u /**< 7-bit SCCB address when SID is strapped to AVDD. */

/** Chip-ID register pair (16-bit register addressing). */
#define OV9281_REG_CHIP_ID_HI 0x300Au /**< High byte of the chip-ID word. */
#define OV9281_REG_CHIP_ID_LO 0x300Bu /**< Low byte of the chip-ID word. */
#define OV9281_CHIP_ID        0x9281u /**< Expected combined CHIP_ID_HI:CHIP_ID_LO value. */

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed SCCB/I²C bus; not owned, not closed by deinit. */
	uint8_t    addr;        /**< 7-bit SCCB address bound at init (LOW or HIGH per SID strap). */
	bool       initialised; /**< True once init verified the chip ID. */
} ov9281_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify the chip ID.
 *
 * @param dev       Output: caller-allocated driver context.
 * @param bus       I²C bus handle (borrowed, must outlive @p dev).
 * @param i2c_addr  7-bit SCCB address (@ref OV9281_I2C_ADDR_LOW or @ref OV9281_I2C_ADDR_HIGH).
 * @return `ALP_OK` on chip-ID match; `ALP_ERR_INVAL` on NULL args;
 *         `ALP_ERR_IO` on chip-ID mismatch; propagated I²C error on bus failure.
 */
alp_status_t ov9281_init(ov9281_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the combined CHIP_ID word.  Useful as a liveness probe.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Output: combined 16-bit chip ID.
 * @return `ALP_OK` on success; `ALP_ERR_NOT_READY` if init did not succeed;
 *         `ALP_ERR_IO` on SCCB failure.
 */
alp_status_t ov9281_read_id(ov9281_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (system-reset bit at 0x0103).
 *
 * @param dev  Initialised driver context.
 * @return `ALP_OK` on success; `ALP_ERR_NOT_READY` on uninitialised driver;
 *         propagated I²C error.
 */
alp_status_t ov9281_soft_reset(ov9281_t *dev);

/**
 * @brief Release the driver context.  Idempotent.
 *
 * Does NOT power down the chip and does not close the borrowed I²C bus.
 *
 * @param dev  Driver context.  NULL is tolerated.
 */
void ov9281_deinit(ov9281_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OV9281_H */
