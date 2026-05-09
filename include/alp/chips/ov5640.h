/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ov5640.h
 * @brief OmniVision OV5640 5 MP CMOS image sensor — config-side driver.
 *
 * Public surface consumed by alp-studio block `blk_camera_ov5640`.
 * Symbols carry the chip's natural prefix `ov5640_*` — no `alp_`.
 *
 * **Scope split.**  The OV5640 has two interfaces:
 *
 *   1. **SCCB (I²C-compatible)** — used for chip-ID readback,
 *      resolution / format / exposure configuration.  This driver
 *      covers that side.
 *   2. **MIPI CSI-2 / DVP parallel** — pixel data egress.  That path
 *      is handled by the SoC's camera receiver and lives behind
 *      `<alp/camera.h>` (real impl arrives in v0.2).  This driver
 *      does not touch it.
 *
 * v0.2 scope: chip-ID verify, software reset, resolution presets
 * (QVGA/VGA/720p/1080p), pixel-format selection (RGB565 / YUV422 /
 * JPEG).  Auto-exposure, auto-white-balance, and night-mode tuning
 * land in v0.3.
 *
 * Datasheet: OmniVision OV5640 (Rev 1.20, Apr 2011).
 */

#ifndef ALP_CHIPS_OV5640_H
#define ALP_CHIPS_OV5640_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SCCB 7-bit address (fixed; OV5640 does not strap selectable). */
#define OV5640_I2C_ADDR        0x3C

/** Chip-ID register pair (16-bit register addressing). */
#define OV5640_REG_CHIP_ID_HI  0x300A
#define OV5640_REG_CHIP_ID_LO  0x300B

/** Combined chip-ID value (CHIP_ID_HI:CHIP_ID_LO). */
#define OV5640_CHIP_ID         0x5640

/** Resolution presets the driver knows how to configure. */
typedef enum {
    OV5640_RES_QVGA  = 0,    /**< 320 × 240 */
    OV5640_RES_VGA   = 1,    /**< 640 × 480 */
    OV5640_RES_720P  = 2,    /**< 1280 × 720 */
    OV5640_RES_1080P = 3,    /**< 1920 × 1080 */
    OV5640_RES_5MP   = 4     /**< 2592 × 1944 (still capture). */
} ov5640_resolution_t;

/** Pixel-format selection. */
typedef enum {
    OV5640_FMT_RGB565  = 0,
    OV5640_FMT_YUV422  = 1,
    OV5640_FMT_JPEG    = 2,
    OV5640_FMT_RAW8    = 3
} ov5640_format_t;

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t           *bus;
    uint8_t              addr;          /**< 7-bit SCCB address. */
    ov5640_resolution_t  res;
    ov5640_format_t      fmt;
    bool                 initialised;
} ov5640_t;

/**
 * @brief Bind a driver context to an open I2C bus and verify the chip ID.
 *
 * Reads CHIP_ID_HI / CHIP_ID_LO and verifies the combined word matches
 * @ref OV5640_CHIP_ID.  Does not change resolution or format — call
 * @ref ov5640_set_resolution and @ref ov5640_set_format next.
 *
 * @return ALP_OK on success; ALP_ERR_IO on chip-ID mismatch.
 */
alp_status_t ov5640_init(ov5640_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read CHIP_ID for liveness checks. */
alp_status_t ov5640_read_id(ov5640_t *dev, uint16_t *id_out);

/** Issue a software reset (writes the system-reset bit at 0x3008). */
alp_status_t ov5640_soft_reset(ov5640_t *dev);

/** Apply a resolution preset (configures DVP / ISP / scaler regs). */
alp_status_t ov5640_set_resolution(ov5640_t *dev, ov5640_resolution_t res);

/** Apply a pixel-format preset (configures format-control regs). */
alp_status_t ov5640_set_format(ov5640_t *dev, ov5640_format_t fmt);

/** Enable / disable test-pattern output (colour-bar) for bring-up. */
alp_status_t ov5640_set_test_pattern(ov5640_t *dev, bool enabled);

/** Release the driver context.  Does not power down the chip. */
void         ov5640_deinit(ov5640_t *dev);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_OV5640_H */
