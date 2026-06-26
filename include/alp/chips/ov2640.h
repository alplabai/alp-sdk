/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ov2640.h
 * @brief OmniVision OV2640 2 MP UXGA CMOS image sensor — config-side driver.
 *
 * Public surface for the OV2640 as deployed on the ESP32-CAM and
 * countless Chinese-domestic UXGA camera modules.  Symbols carry
 * the chip's natural prefix `ov2640_*` — no `alp_`.
 *
 * @par Scope split
 *   1. **SCCB (I²C-compatible, fixed 0x30 7-bit)** — chip-ID readback,
 *      resolution / format / brightness / colour-bar config.  This
 *      driver covers that side.
 *   2. **DVP 8-bit parallel** — pixel egress.  Routed to the SoC's
 *      camera receiver behind `<alp/camera.h>`; this driver does
 *      not touch it.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *   First SoM verification on V2N's GD32 bridge DVP path arrives
 *   in v0.5.x.  The lifecycle / setter API below is stable; pixel
 *   path bring-up may add helpers without breaking these symbols.
 *
 * @par Driver status: [stub-impl]
 *   Resolution / format setters store the request and validate the
 *   chip-ID; the OmniVision-vendor 600+ register init table for
 *   each preset lands in a follow-up once the maintainer adds the
 *   reference init script to the internal design archive.  Until
 *   then the setters return `ALP_ERR_NOSUPPORT` after stashing the
 *   request, mirroring the existing `ov5640` shape.
 *
 * Datasheet: OmniVision OV2640 (Rev 2.2, Apr 2006).
 */

#ifndef ALP_CHIPS_OV2640_H
#define ALP_CHIPS_OV2640_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SCCB 7-bit address (fixed; OV2640 does not strap selectable). */
#define OV2640_I2C_ADDR 0x30u

/** Bank-select pseudo-register (selects DSP / SENSOR register pages). */
#define OV2640_REG_BANK_SEL 0xFFu
#define OV2640_BANK_DSP     0x00u
#define OV2640_BANK_SENSOR  0x01u

/** PIDH:PIDL on bank SENSOR = (0x26, 0x42) -- combined `OV2640_CHIP_ID`. */
#define OV2640_REG_PIDH 0x0Au
#define OV2640_REG_PIDL 0x0Bu
#define OV2640_CHIP_ID  0x2642u

/** Resolution presets the driver knows how to configure. */
typedef enum {
	OV2640_RES_QQVGA = 0, /**< 160 × 120 */
	OV2640_RES_QVGA  = 1, /**< 320 × 240 */
	OV2640_RES_VGA   = 2, /**< 640 × 480 */
	OV2640_RES_SVGA  = 3, /**< 800 × 600 */
	OV2640_RES_XGA   = 4, /**< 1024 × 768 */
	OV2640_RES_SXGA  = 5, /**< 1280 × 1024 */
	OV2640_RES_UXGA  = 6, /**< 1600 × 1200 (full-res) */
} ov2640_resolution_t;

/** Pixel-format selection. */
typedef enum {
	OV2640_FMT_RGB565 = 0, /**< 16-bit RGB565, big-endian on the DVP bus. */
	OV2640_FMT_YUV422 = 1, /**< 16-bit YUV422 (YUYV order). */
	OV2640_FMT_JPEG   = 2, /**< On-chip JPEG-compressed stream. */
} ov2640_format_t;

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t          *bus;  /**< Borrowed SCCB/I²C bus; not owned, not closed by deinit. */
	uint8_t             addr; /**< 7-bit SCCB address bound at init (@ref OV2640_I2C_ADDR). */
	ov2640_resolution_t res;  /**< Last requested resolution preset (stashed, may be unapplied). */
	ov2640_format_t     fmt;  /**< Last requested pixel format (stashed, may be unapplied). */
	bool                initialised; /**< True once init verified the chip ID. */
} ov2640_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify the chip ID.
 *
 * Selects the SENSOR bank, reads PIDH:PIDL, compares against
 * @ref OV2640_CHIP_ID.
 *
 * @param dev       Output: caller-allocated driver context.
 * @param bus       I²C bus handle from `alp_i2c_open`.
 * @param i2c_addr  7-bit SCCB address (always @ref OV2640_I2C_ADDR).
 * @return `ALP_OK` on chip-ID match, `ALP_ERR_INVAL` on bad args,
 *         `ALP_ERR_IO` on chip-ID mismatch, propagated I²C error
 *         on bus failure.
 */
alp_status_t ov2640_init(ov2640_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read combined PIDH:PIDL.  Useful as a liveness probe.
 *
 * @param dev     Initialised driver context.
 * @param id_out  Output: combined 16-bit chip ID.
 * @return `ALP_OK` on success, `ALP_ERR_NOT_READY` if `init` did
 *         not succeed, `ALP_ERR_IO` on SCCB failure.
 */
alp_status_t ov2640_read_id(ov2640_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (DSP-bank 0x12 bit 7).
 *
 * @param dev  Initialised driver context.
 * @return `ALP_OK` on success, `ALP_ERR_NOT_READY` on uninitialised
 *         driver, propagated I²C error.
 */
alp_status_t ov2640_soft_reset(ov2640_t *dev);

/**
 * @brief Remember a resolution preset (v0.5 stub: stashes only).
 *
 * @param dev  Initialised driver context.
 * @param res  Resolution enum value.
 * @return `ALP_ERR_INVAL` on out-of-range enum, `ALP_ERR_NOSUPPORT`
 *         after stashing (vendor init table pending).
 */
alp_status_t ov2640_set_resolution(ov2640_t *dev, ov2640_resolution_t res);

/**
 * @brief Remember a pixel-format preset (v0.5 stub: stashes only).
 *
 * @param dev  Initialised driver context.
 * @param fmt  Format enum value.
 * @return `ALP_ERR_INVAL` on out-of-range enum, `ALP_ERR_NOSUPPORT`
 *         after stashing.
 */
alp_status_t ov2640_set_format(ov2640_t *dev, ov2640_format_t fmt);

/**
 * @brief Enable/disable the colour-bar test pattern.
 *
 * @param dev      Initialised driver context.
 * @param enabled  `true` to enable the static colour-bar pattern.
 * @return `ALP_OK` on success, `ALP_ERR_NOT_READY` on uninitialised
 *         driver, `ALP_ERR_NOSUPPORT` until the vendor pattern-reg
 *         map lands.
 */
alp_status_t ov2640_set_test_pattern(ov2640_t *dev, bool enabled);

/**
 * @brief Release the driver context.  Idempotent.
 *
 * Does NOT power down the chip — the host owns power-rail
 * sequencing (PWDN / RESETB pads).
 *
 * @param dev  Driver context.  NULL is tolerated.
 */
void ov2640_deinit(ov2640_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OV2640_H */
