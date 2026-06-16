/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ov7670.h
 * @brief OmniVision OV7670 VGA CMOS image sensor — config-side driver.
 *
 * Classic VGA-class DVP camera widely cloned in entry-level kit.
 * SCCB config side only; pixel data flows over the DVP 8-bit
 * parallel bus to the SoC camera receiver.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * @par Driver status: [stub-impl]
 *   Chip-ID verify + soft reset + setter stash.  Vendor format-
 *   register table land once the DVP receiver path is brought up.
 *
 * Datasheet: OmniVision OV7670 (Rev 1.01, Jul 2005).
 */

#ifndef ALP_CHIPS_OV7670_H
#define ALP_CHIPS_OV7670_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SCCB 7-bit address (fixed). */
#define OV7670_I2C_ADDR 0x21u

/** Chip-ID register pair (PID is 0x76; VER is 0x73). */
#define OV7670_REG_PID 0x0Au
#define OV7670_REG_VER 0x0Bu
#define OV7670_CHIP_ID 0x7673u /**< Combined PID:VER. */

/** Resolution presets. */
typedef enum {
	OV7670_RES_QQVGA = 0, /**< 160 × 120 */
	OV7670_RES_QVGA  = 1, /**< 320 × 240 */
	OV7670_RES_VGA   = 2, /**< 640 × 480 */
} ov7670_resolution_t;

/** Pixel-format selection. */
typedef enum {
	OV7670_FMT_RGB565 = 0,
	OV7670_FMT_YUV422 = 1,
	OV7670_FMT_BAYER  = 2,
} ov7670_format_t;

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t          *bus;
	uint8_t             addr;
	ov7670_resolution_t res;
	ov7670_format_t     fmt;
	bool                initialised;
} ov7670_t;

/**
 * @brief Bind context to bus and verify chip ID.
 *
 * @param dev       Output: caller-allocated driver context.
 * @param bus       I²C bus handle.
 * @param i2c_addr  7-bit SCCB address.
 * @return `ALP_OK` on chip-ID match.
 */
alp_status_t ov7670_init(ov7670_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read CHIP_ID for liveness.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Output: combined PID:VER.
 * @return `ALP_OK` on success.
 */
alp_status_t ov7670_read_id(ov7670_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (COM7 bit 7).
 *
 * @param dev  Initialised driver context.
 * @return `ALP_OK` on success.
 */
alp_status_t ov7670_soft_reset(ov7670_t *dev);

/**
 * @brief Remember a resolution preset (v0.5 stub).
 *
 * @param dev  Initialised driver context.
 * @param res  Resolution enum.
 * @return `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT`.
 */
alp_status_t ov7670_set_resolution(ov7670_t *dev, ov7670_resolution_t res);

/**
 * @brief Remember a pixel format (v0.5 stub).
 *
 * @param dev  Initialised driver context.
 * @param fmt  Format enum.
 * @return `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT`.
 */
alp_status_t ov7670_set_format(ov7670_t *dev, ov7670_format_t fmt);

/**
 * @brief Release driver context.  Idempotent.
 *
 * @param dev  Driver context.  NULL tolerated.
 */
void ov7670_deinit(ov7670_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OV7670_H */
