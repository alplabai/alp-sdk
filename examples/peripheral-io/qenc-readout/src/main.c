/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * qenc-readout — open the rotary encoder and poll the accumulated
 * position for ~1 second.
 *
 * Quadrature encoders are the standard interface for rotary
 * controls (volume knobs, position dials, motor feedback).  Two
 * out-of-phase signals (A and B) let the SoC's QDEC peripheral
 * detect direction as well as count -- positive ticks for one
 * direction, negative for the other.
 *
 * The E1M spec reserves four encoders (ENC0..ENC3), each routed
 * as a complementary pad pair (ENCn_X / ENCn_Y).  Apps that only
 * use E1M_ENCn for n < E1M_ENC_COUNT (= 4) stay portable
 * across every E1M-conformant SoM.
 *
 * Runs on both EVKs: BOARD_ENC_ROTARY (from <alp/board.h>) resolves
 * to E1M_ENC0 on E1M EVK (PEC12R-4222F-S0024, 24 PPR) and
 * E1M_X_ENC0 on E1M-X EVK (PEC12R-4222F, same form factor).
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/counter.h"

/* BOARD_ENC_ROTARY is the portable alias from <alp/board.h>
 * (E1M_ENC0 on E1M EVK; E1M_X_ENC0 on E1M-X EVK). */
#include "alp/board.h"

int main(void)
{
    printf("[qenc] open BOARD_ENC_ROTARY\n");

    alp_qenc_t *enc = alp_qenc_open(&(alp_qenc_config_t){
        /* BOARD_ENC_ROTARY resolves to E1M_ENC0 on E1M EVK and
         * E1M_X_ENC0 on E1M-X EVK -- an index into the alp-qenc<N>
         * DT alias table; the SoC binds it to a specific QDEC peripheral. */
        .encoder_id = BOARD_ENC_ROTARY,
        /* Mechanical resolution -- informational only at the
         * wrapper level.  24 PPR is typical for a Bourns PEC11R
         * panel-mount knob; AS5048A magnetic encoders are 14-bit
         * (16384 PPR).  Apps use this hint to scale "ticks per
         * full rotation". */
        .pulses_per_rev = 24,
    });
    if (enc == NULL) {
        printf("[qenc] open failed: alp_last_error=%d\n", (int)alp_last_error());
        printf("[qenc] done\n");
        return 0;
    }

    /* Reset the accumulator so we start from zero.  Without this,
     * the position read first reflects whatever rotation
     * happened before we opened the handle. */
    alp_status_t s = alp_qenc_reset_position(enc);
    printf("[qenc] reset_position -> %d\n", (int)s);

    /* Poll the position 10 times at 100 ms intervals = 1 s of
     * sampling.  Real apps would use either:
     *   - A k_timer with a higher rate (10 ms) for low-latency UI.
     *   - The Zephyr input subsystem for event-driven dispatch.
     * Polling is simplest for a demo. */
    for (int i = 0; i < 10; i++) {
        int32_t pos = 0;
        s           = alp_qenc_get_position(enc, &pos);
        printf("[qenc] t=%d ms  status=%d  pos=%d\n", (i + 1) * 100, (int)s, (int)pos);
        k_msleep(100);
    }

    /* Close releases the handle.  The hardware QDEC keeps running
     * -- if you reopen with the same encoder_id, you'll read the
     * accumulated position as it was when you closed (modulo any
     * intervening rotation). */
    alp_qenc_close(enc);
    printf("[qenc] done\n");
    return 0;
}
