/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tsl2591.h
 * @brief ams TSL2591 wide-dynamic-range ambient-light sensor (I²C).
 *
 * 600 M:1 dynamic range visible + IR light sensor.  Two 16-bit ADC
 * channels (full-spectrum + IR-only) -- visible light is the
 * difference.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ams TSL2591 v0.7 (May 2013).
 */

#ifndef ALP_CHIPS_TSL2591_H
#define ALP_CHIPS_TSL2591_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TSL2591_I2C_ADDR 0x29u

#define TSL2591_CMD_BIT 0xA0u
#define TSL2591_REG_ENABLE 0x00u
#define TSL2591_REG_ID 0x12u
#define TSL2591_REG_C0DATA_LO 0x14u

#define TSL2591_DEVICE_ID 0x50u

#define TSL2591_ENABLE_POWER_ON 0x01u
#define TSL2591_ENABLE_AEN 0x02u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} tsl2591_t;

/** @brief Bind context and verify chip ID. */
alp_status_t tsl2591_init(tsl2591_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read 16-bit visible+IR (CH0) and IR-only (CH1) counts. */
alp_status_t tsl2591_read_channels(tsl2591_t *dev, uint16_t *ch0_full_out, uint16_t *ch1_ir_out);

/** @brief Release driver context. */
void tsl2591_deinit(tsl2591_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TSL2591_H */
