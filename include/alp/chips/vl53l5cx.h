/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vl53l5cx.h
 * @brief STMicroelectronics VL53L5CX multi-zone time-of-flight ranger.
 *
 * 8 × 8 multi-zone ToF sensor (64 individually-resolved cells) up to
 * 4 m.  Same lifecycle shape as `vl53l1x` but with the additional
 * 64-cell distance / object-count read on top.  This driver covers
 * chip-ID probe + soft reset; the firmware boot + multi-zone read
 * path land alongside the ST ULD library import.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Driver status: [stub-impl]
 *
 * Datasheet: ST VL53L5CX DS13754 Rev 3 (Apr 2021).
 */

#ifndef ALP_CHIPS_VL53L5CX_H
#define ALP_CHIPS_VL53L5CX_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VL53L5CX_I2C_ADDR_DEFAULT 0x29u
#define VL53L5CX_REG_DEVICE_ID    0x0000u
#define VL53L5CX_DEVICE_ID        0xF0u /**< boot-status low byte after fw load. */

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} vl53l5cx_t;

/** @brief Bind context and probe. */
alp_status_t vl53l5cx_init(vl53l5cx_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read boot-status low byte. */
alp_status_t vl53l5cx_read_id(vl53l5cx_t *dev, uint8_t *id_out);

/** @brief Issue a software reset. */
alp_status_t vl53l5cx_soft_reset(vl53l5cx_t *dev);

/** @brief Release driver context. */
void vl53l5cx_deinit(vl53l5cx_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VL53L5CX_H */
