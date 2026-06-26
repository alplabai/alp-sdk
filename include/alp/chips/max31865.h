/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file max31865.h
 * @brief ADI (Maxim) MAX31865 RTD (PT100 / PT1000) digitiser (SPI).
 *
 * 15-bit RTD-to-digital converter with on-chip Wheatstone bridge
 * + fault detection.  Same family as MAX31855 (thermocouple);
 * different driver because RTD bridge configuration is more
 * involved.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — config-write + RTD-read; full
 *   fault-state machine pending.
 *
 * Datasheet: ADI (Maxim) MAX31865 (Rev 3, Aug 2015).
 */

#ifndef ALP_CHIPS_MAX31865_H
#define ALP_CHIPS_MAX31865_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register addresses (SPI read uses reg; write uses 0x80 | reg). */
#define MAX31865_REG_CONFIG     0x00u /**< Configuration register (RW; write 0x80 | reg). */
#define MAX31865_REG_RTD_MSB    0x01u /**< RTD data high byte (read-only). */
#define MAX31865_REG_RTD_LSB    0x02u /**< RTD data low byte; bit 0 is the fault flag. */
#define MAX31865_REG_FAULT_STAT 0x07u /**< Fault-status register (read-only). */

/* CONFIG register bit fields (OR together for max31865_set_config). */
#define MAX31865_CONFIG_VBIAS        0x80u /**< Enable the RTD bias voltage. */
#define MAX31865_CONFIG_MODE_AUTO    0x40u /**< Auto-conversion mode (continuous). */
#define MAX31865_CONFIG_3WIRE        0x10u /**< 3-wire RTD wiring (clear for 2/4-wire). */
#define MAX31865_CONFIG_FAULT_AUTO   0x04u /**< Run the automatic fault-detection cycle. */
#define MAX31865_CONFIG_CLEAR_FAULTS 0x02u /**< Clear the fault-status register (self-clearing). */
#define MAX31865_CONFIG_FILTER_50HZ  0x01u /**< Select 50 Hz notch filter (clear for 60 Hz). */

/**
 * @brief Driver context for one MAX31865. Treat as opaque; populated by
 *        @ref max31865_init.
 */
typedef struct {
	alp_spi_t *bus;         /**< Borrowed SPI bus; owned by the caller, not freed here. */
	bool       initialised; /**< True once @ref max31865_init has run. */
} max31865_t;

/**
 * @brief Bind a context to a caller-opened SPI bus.
 *
 * Records the bus handle only; does not write CONFIG. The caller must set up
 * the bus (SPI mode 1 or 3, up to 5 MHz) and then configure the device via
 * @ref max31865_set_config before reading.
 *
 * @param dev Context to initialise. Must be non-NULL.
 * @param spi Open SPI bus; borrowed, must outlive @p dev.
 * @return ALP_OK on success, or an error if arguments are invalid.
 */
alp_status_t max31865_init(max31865_t *dev, alp_spi_t *spi);

/**
 * @brief Write the CONFIG register.
 *
 * @param dev    Initialised context.
 * @param config Bitmask of MAX31865_CONFIG_* flags.
 * @return ALP_OK on success, or an error from the SPI transfer.
 */
alp_status_t max31865_set_config(max31865_t *dev, uint8_t config);

/**
 * @brief Read the 15-bit RTD ratio (0..32767).
 *
 * The raw register packs the fault flag in bit 0 of RTD_LSB; it is stripped
 * from @p rtd_out and surfaced separately via @p fault_set. Convert the ratio
 * to resistance with: R_rtd = rtd_out * R_ref / 32768.
 *
 * @param dev       Initialised and configured context.
 * @param rtd_out   Receives the 15-bit ratio with the fault bit removed.
 * @param fault_set Receives true if the fault flag was set; pass NULL to ignore.
 * @return ALP_OK on success, or an error from the SPI transfer.
 */
alp_status_t max31865_read_rtd(max31865_t *dev, uint16_t *rtd_out, bool *fault_set);

/**
 * @brief Release the driver context. Does not close the borrowed SPI bus.
 *
 * @param dev Context to release; NULL is ignored.
 */
void max31865_deinit(max31865_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX31865_H */
