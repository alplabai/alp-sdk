/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ov5645.h
 * @brief OmniVision OV5645 5 MP MIPI CSI-2 image sensor — config-side driver.
 *
 * MIPI CSI-2 5 MP camera popular as the "RPi-grade colour camera"
 * on industrial carriers.  This driver covers the SCCB (I²C-
 * compatible) config side only; pixel data egress flows over
 * MIPI CSI-2 D-PHY lanes and is handled by the SoC's camera
 * receiver behind `<alp/camera.h>`.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *
 * @par Driver status: [stub-impl]
 *   Chip-ID verify + soft reset + setter stash.  Vendor MIPI-CSI-2
 *   lane-count + resolution register tables land once MIPI bring-up
 *   on V2N's RX path stabilises.
 *
 * Datasheet: OmniVision OV5645 (Rev 1.0, Sep 2014).
 */

#ifndef ALP_CHIPS_OV5645_H
#define ALP_CHIPS_OV5645_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SCCB 7-bit address (fixed). */
#define OV5645_I2C_ADDR 0x3Cu

/** Chip-ID register pair (16-bit register addressing). */
#define OV5645_REG_CHIP_ID_HI 0x300Au
#define OV5645_REG_CHIP_ID_LO 0x300Bu
#define OV5645_CHIP_ID        0x5645u

/** Resolution presets. */
typedef enum {
    OV5645_RES_VGA   = 0, /**< 640 × 480   */
    OV5645_RES_720P  = 1, /**< 1280 × 720  */
    OV5645_RES_1080P = 2, /**< 1920 × 1080 */
    OV5645_RES_5MP   = 3, /**< 2592 × 1944 */
} ov5645_resolution_t;

/** MIPI CSI-2 lane count. */
typedef enum {
    OV5645_LANES_1 = 1,
    OV5645_LANES_2 = 2,
} ov5645_lanes_t;

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t          *bus;
    uint8_t             addr;
    ov5645_resolution_t res;
    ov5645_lanes_t      lanes;
    bool                initialised;
} ov5645_t;

/**
 * @brief Bind a driver context and verify the chip ID.
 *
 * @param dev       Output: caller-allocated driver context.
 * @param bus       I²C bus handle.
 * @param i2c_addr  7-bit SCCB address (typically @ref OV5645_I2C_ADDR).
 * @return `ALP_OK` on success.
 */
alp_status_t ov5645_init(ov5645_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read CHIP_ID for liveness checks.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Output: combined 16-bit chip ID.
 * @return `ALP_OK` on success.
 */
alp_status_t ov5645_read_id(ov5645_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset.
 *
 * @param dev  Initialised driver context.
 * @return `ALP_OK` on success.
 */
alp_status_t ov5645_soft_reset(ov5645_t *dev);

/**
 * @brief Remember a resolution preset (v0.5 stub: stashes only).
 *
 * @param dev  Initialised driver context.
 * @param res  Resolution enum value.
 * @return `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT`.
 */
alp_status_t ov5645_set_resolution(ov5645_t *dev, ov5645_resolution_t res);

/**
 * @brief Remember a MIPI lane-count preset (v0.5 stub: stashes only).
 *
 * @param dev    Initialised driver context.
 * @param lanes  Lane count (1 or 2).
 * @return `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT`.
 */
alp_status_t ov5645_set_lanes(ov5645_t *dev, ov5645_lanes_t lanes);

/**
 * @brief Release the driver context.  Idempotent.
 *
 * @param dev  Driver context.  NULL is tolerated.
 */
void ov5645_deinit(ov5645_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OV5645_H */
