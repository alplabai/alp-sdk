/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2s-tone — stream a triangle-wave PCM buffer to ALP_E1M_I2S0.
 * Demonstrates the open / start / write / stop / close lifecycle.
 *
 * Uses a triangle wave (cheap integer math) rather than a true
 * sine so the example doesn't need libm linked in -- the tone
 * is still audible on a real DAC and the code works on all
 * baremetal/Zephyr toolchains.
 */

#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/i2s.h"
#include "alp/e1m_pinout.h"

#define SR             48000u   /* sample rate */
#define BLOCK_FRAMES   256u
#define BLOCKS_TO_SEND 4u

int main(void) {
    printf("[i2s] open ALP_E1M_I2S0 @ 48 kHz s16 stereo TX\n");

    alp_i2s_t *i2s = alp_i2s_open(&(alp_i2s_config_t){
        .bus_id          = ALP_E1M_I2S0,
        .sample_rate_hz  = SR,
        .word_bits       = 16,
        .channels        = 2,
        .format          = ALP_I2S_FMT_I2S,
        .direction       = ALP_I2S_DIR_TX,
        .block_frames    = BLOCK_FRAMES,
    });
    if (i2s == NULL) {
        printf("[i2s] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[i2s] done\n");
        return 0;
    }

    alp_status_t s = alp_i2s_start(i2s);
    printf("[i2s] start -> %d\n", (int)s);

    /* Pre-compute one block of stereo triangle wave.  PERIOD samples
     * per cycle ⇒ 48000/PERIOD Hz tone — pick PERIOD=48 for 1 kHz. */
    static int16_t block[BLOCK_FRAMES * 2];   /* L,R interleaved */
    enum { PERIOD = 48 };
    enum { AMP = 16384 };
    for (uint32_t i = 0; i < BLOCK_FRAMES; i++) {
        uint32_t p = i % PERIOD;
        int32_t v = (p < PERIOD / 2)
            ? ((int32_t)p * (4 * AMP) / PERIOD - AMP)
            : (3 * AMP - (int32_t)p * (4 * AMP) / PERIOD);
        block[i * 2 + 0] = (int16_t)v;     /* L */
        block[i * 2 + 1] = (int16_t)v;     /* R */
    }

    for (uint32_t b = 0; b < BLOCKS_TO_SEND; b++) {
        s = alp_i2s_write(i2s, block, sizeof block, 100);
        printf("[i2s] write block %u -> %d\n", b, (int)s);
        if (s != ALP_OK) break;
    }

    alp_i2s_stop(i2s);
    alp_i2s_close(i2s);
    printf("[i2s] done\n");
    return 0;
}
