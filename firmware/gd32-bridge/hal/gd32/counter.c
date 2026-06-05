/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- free-running counter.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

int bridge_hw_counter_read(uint8_t counter, uint32_t *ticks)
{
    if (ticks == 0) return BRIDGE_HW_ERR_INVAL;
    *ticks = 0u;
    /* Single free-running counter exposed today; future revisions can
     * carve out additional ids for derived (slower) tick bases.  The
     * DWT counter ticks at the core clock (216 MHz on GD32G553),
     * wraps every ~19.9 s, and is monotonically non-decreasing across
     * reads -- the host can compute deltas without watching for
     * mid-read consistency since the register is atomic. */
    if (counter != 0u) return BRIDGE_HW_ERR_RANGE;
    *ticks = DWT->CYCCNT;
    return BRIDGE_HW_OK;
}
