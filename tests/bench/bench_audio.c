/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/audio.h>.  v0.3 covers the rejection /
 * fast-path costs that apps probing for PDM input or I2S output
 * pay when the audio path isn't wired
 * (CONFIG_ALP_SDK_AUDIO_IN=n / CONFIG_ALP_SDK_AUDIO_OUT=n).  v1.0
 * adds steady-state benches (per-block DC-block + gain + write
 * latency) once the HW-in-loop audio loopback runner ships.
 */

#include "bench.h"

#include "alp/audio.h"

void bench_audio_main(void)
{
    /* PDM input NULL-cfg rejection. */
    BENCH_RUN("alp_audio_in_open(NULL)", 1000000, { (void)alp_audio_in_open(NULL); });

    /* PDM input with an empty cfg -- exercises the struct-touch
     * cost in the stub backend.  Real path will validate
     * peripheral_id + sample_rate against soc_caps. */
    BENCH_RUN("alp_audio_in_open(empty cfg)", 1000000, {
        alp_audio_config_t cfg = {0};
        (void)alp_audio_in_open(&cfg);
    });

    /* I2S output NULL-cfg rejection. */
    BENCH_RUN("alp_audio_out_open(NULL)", 1000000, { (void)alp_audio_out_open(NULL); });

    BENCH_RUN("alp_audio_out_open(empty cfg)", 1000000, {
        alp_audio_config_t cfg = {0};
        (void)alp_audio_out_open(&cfg);
    });
}
