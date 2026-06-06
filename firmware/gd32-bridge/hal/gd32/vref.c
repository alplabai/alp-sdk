/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- analog reference readiness latch.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* Analog-reference health latch.  bridge_hw_init arms the on-chip
 * reference buffer and records whether VREFRDY ever set; every
 * ADC/DAC entry point consults this before touching its converter.
 * A reference that never locked means EVERY conversion result is
 * garbage referenced to a dead node -- the v0.2.6 root cause -- and a
 * STATUS_OK reply carrying garbage millivolts is indistinguishable on
 * the wire from healthy analog (no GET_FAULT-style opcode exists).
 * Failing the analog ops with an IO error is the only honest signal
 * this protocol revision can give.
 *
 * The check self-heals: a buffer that locks LATE (after init's bounded
 * wait expired) is promoted on the first analog op that finds VREFRDY
 * set -- same lazy-readiness shape the TRNG bring-up uses. */
bool vref_ok = false;

bool vref_ready_check(void)
{
    if (!vref_ok) {
        vref_ok = (vref_status_get() == SET);
    }
    return vref_ok;
}
