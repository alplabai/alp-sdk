/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hailo_8l.h
 * @brief Hailo-8L 13 TOPS NPU (M.2 PCIe) host-side bring-up surface.
 *
 * The Hailo-8L is a 13 TOPS edge AI accelerator delivered as an
 * M.2 2242 / 2230 PCIe card.  The on-host inference runtime
 * (`HailoRT`, Apache-2.0) talks to the device over PCIe Gen3 x4.
 * This driver only covers the *host-side* power-rail + reset +
 * heartbeat surface that an embedded board needs before the
 * Linux PCIe enumeration sees the device.
 *
 * @par Scope split
 *   - **This driver**: host GPIO sequence (RESETB pulse, PCIe-WAKE
 *     line monitor, PCIe-CLKREQ pacing), I²C-via-side-band probe
 *     of the on-board EEPROM/temp-sensor for board variant readback.
 *   - **NOT this driver**: the inference runtime, model loader,
 *     PCIe driver, DMA engine.  Those live on the Linux side as
 *     HailoRT plus the upstream `hailo_pci` kernel module.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — defines lifecycle + GPIO
 *   sequencing surface; live silicon probe arrives once a board
 *   ships with the M.2 slot wired.
 *
 * Datasheet: Hailo-8L product brief Rev 1.2 (2024); pin map from
 * Hailo Embedded Reference Design.
 */

#ifndef ALP_CHIPS_HAILO_8L_H
#define ALP_CHIPS_HAILO_8L_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	alp_gpio_t *resetb;  /**< Open-drain reset to the M.2 slot. */
	alp_gpio_t *pe_wake; /**< PCIe WAKE# input from the slot. */
	bool        initialised;
} hailo_8l_t;

/**
 * @brief Bind context to host GPIO surface; does NOT enumerate PCIe.
 *
 * Drives RESETB low for at least 100 ms then releases it, leaving
 * the M.2 card in the "freshly reset, awaiting PCIe enumeration"
 * state.  The host PCIe root complex (not this driver) brings the
 * link up once Linux sees the slot.
 *
 * @param dev      Output: caller-allocated context.
 * @param resetb   Host RESETB GPIO (active low).
 * @param pe_wake  Host PCIe WAKE# input from the M.2 slot.
 * @return `ALP_OK` on success.
 */
alp_status_t hailo_8l_init(hailo_8l_t *dev, alp_gpio_t *resetb, alp_gpio_t *pe_wake);

/**
 * @brief Assert RESETB then release.  100 ms low pulse.
 *
 * @param dev  Initialised context.
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL on NULL/uninitialised
 *         dev, else the underlying GPIO error.
 */
alp_status_t hailo_8l_reset(hailo_8l_t *dev);

/**
 * @brief Sample the PCIe WAKE# line.
 *
 * @param dev            Initialised context.
 * @param wake_asserted  Out: true when WAKE# is asserted (active-low low).
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL on NULL args, else
 *         the underlying GPIO error.
 */
alp_status_t hailo_8l_read_wake(hailo_8l_t *dev, bool *wake_asserted);

/**
 * @brief Release driver context.  Does NOT close the GPIO handles.
 *
 * @param dev  Context to tear down (NULL is tolerated).
 */
void hailo_8l_deinit(hailo_8l_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_HAILO_8L_H */
