/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tmc2209.h
 * @brief Trinamic TMC2209 silent stepper driver (UART-controlled).
 *
 * Step/dir + UART-tunable bipolar stepper driver dominant in the
 * desktop-3D-printer ecosystem.  This driver covers the UART
 * register-access surface; step/dir pacing reuses the
 * `<alp/pwm.h>` portable layer the way `drv8825` already does.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — UART CRC + register-read /
 *   register-write helpers.  GCONF / IHOLD_IRUN / CHOPCONF setters
 *   land in follow-up commits.
 *
 * Datasheet: Trinamic TMC2209 v1.09 (Aug 2020).
 */

#ifndef ALP_CHIPS_TMC2209_H
#define ALP_CHIPS_TMC2209_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Driver context for one TMC2209 on a (possibly shared) UART. */
typedef struct {
	alp_uart_t *port;        /**< Borrowed UART port; not owned. */
	uint8_t     slave_addr;  /**< 0..3 (MS1 + MS2 strap-selected). */
	bool        initialised; /**< True once tmc2209_init() ran. */
} tmc2209_t;

/**
 * @brief Bind context to a caller-opened UART port.
 *
 * Does not touch the chip; the first register access does.  Several
 * TMC2209s may share one half-duplex UART, distinguished by @p slave_addr.
 *
 * @param dev         Caller-allocated context; populated on success.
 * @param port        Caller-opened UART port; borrowed, must outlive @p dev.
 * @param slave_addr  Node address 0..3 (set by the MS1/MS2 straps).
 * @return ALP_OK; ALP_ERR_INVAL if @p dev / @p port is NULL or
 *         @p slave_addr > 3.
 */
alp_status_t tmc2209_init(tmc2209_t *dev, alp_uart_t *port, uint8_t slave_addr);

/**
 * @brief Read a 32-bit TMC2209 register over the UART datagram protocol.
 *
 * Sends a read request, then validates the reply's sync byte and CRC.
 *
 * @param dev      Initialised context.
 * @param reg      Register address (datasheet register map).
 * @param val_out  Receives the 32-bit register value (big-endian on the wire).
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; ALP_ERR_INVAL if
 *         @p val_out is NULL; ALP_ERR_IO on bad sync byte / CRC mismatch;
 *         the underlying UART error otherwise.
 */
alp_status_t tmc2209_read_reg(tmc2209_t *dev, uint8_t reg, uint32_t *val_out);

/**
 * @brief Write a 32-bit TMC2209 register over the UART datagram protocol.
 *
 * Write datagrams are fire-and-forget (no reply); the chip's IFCNT
 * register can be polled separately to confirm the write landed.
 *
 * @param dev  Initialised context.
 * @param reg  Register address (datasheet register map).
 * @param val  32-bit value to write (sent big-endian).
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying UART
 *         error otherwise.
 */
alp_status_t tmc2209_write_reg(tmc2209_t *dev, uint8_t reg, uint32_t val);

/**
 * @brief Release the driver context (clears @c initialised).
 *
 * Does not touch the borrowed UART port.  NULL tolerated.
 *
 * @param dev  Context to release, or NULL.
 */
void tmc2209_deinit(tmc2209_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TMC2209_H */
