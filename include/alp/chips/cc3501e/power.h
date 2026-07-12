/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file power.h
 * @brief CC3501E power-management policy host helper (opcode 0x62).
 */

#ifndef ALP_CHIPS_CC3501E_POWER_H
#define ALP_CHIPS_CC3501E_POWER_H

#include <stdint.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply a power-management policy hint to the CC3501E (POWER_POLICY, 0x62).
 *
 * Hints how aggressively the CC3501E firmware idles between events.  The coarse
 * preset maps to the CC35xx Power manager: PERFORMANCE keeps the device awake
 * (WFI only), BALANCED lets it opportunistically sleep between events, and
 * LOW_POWER / DEEP_SLEEP let it reach its deepest sleep state.  Takes effect on
 * the device's next idle-detection cycle.
 *
 * @param ctx         Initialised bridge handle.
 * @param policy      Power-policy payload: @c policy is one of
 *                    @ref alp_cc3501e_pp_preset_t; @c wake_events is a bitmap of
 *                    @c ALP_CC3501E_WAKE_* (all-zero is rejected with a low-power
 *                    preset -- keep at least @c ALP_CC3501E_WAKE_HOST_SPI);
 *                    @c idle_ms_before_sleep is the minimum idle before sleep
 *                    (0 = firmware default).  @c reserved is sent as zero.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p policy is NULL, the preset is
 *         unknown, or a low-power preset carries no wake source; otherwise the
 *         mapped error.
 */
alp_status_t
cc3501e_power_policy(cc3501e_t *ctx, const alp_cc3501e_power_policy_t *policy, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_POWER_H */
