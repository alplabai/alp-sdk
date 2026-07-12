/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.h
 * @brief CC3501E camera-enable LDO host helper (opcodes 0x60/0x61).
 *
 * The CC3501E fronts the two on-module camera-enable LDOs; this helper lets
 * the host drive them over the inter-chip bridge.  Synchronous + fast in the
 * firmware (no worker, no radio bring-up), so it takes the caller's timeout
 * with no down-window floor -- but it still retries on a transient IO (the
 * bridge is briefly down if a radio op is running concurrently).
 */

#ifndef ALP_CHIPS_CC3501E_CAMERA_H
#define ALP_CHIPS_CC3501E_CAMERA_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/chips/cc3501e/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drive a CC3501E camera-enable LDO (CAM_ENABLE 0x60 / CAM_DISABLE 0x61).
 *
 * @param ctx         Initialised bridge handle.
 * @param which       0 = CAM_EN_LDO0 (CC35 GPIO_1), 1 = CAM_EN_LDO1 (CC35 GPIO_0)
 *                    — per the E1M-AEN BDE-BW35N U4 netlist (pins 54/55).
 * @param on          true asserts the enable, false deasserts.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL on a bad @p which; otherwise mapped.
 */
alp_status_t cc3501e_cam_enable(cc3501e_t *ctx, uint8_t which, bool on, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_CAMERA_H */
