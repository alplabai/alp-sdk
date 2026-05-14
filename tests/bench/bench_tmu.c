/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/tmu.h>.  Measures the math-primitive
 * dispatch cost: on native_sim builds (no V2N supervisor) the call
 * routes through libm `sinf`/`cosf`/... directly, so these numbers
 * are an upper bound on the SDK's per-call dispatcher overhead.
 *
 * Real-silicon V2N baselines (CMD_TMU_COMPUTE round-trip latency at
 * each transport bus speed) live in the per-(SoM, OS) baselines
 * under E1M-V2N101-zephyr.yaml etc.
 */

#include <stddef.h>

#include "bench.h"

#include "alp/tmu.h"

void bench_tmu_main(void)
{
    /* Each primitive runs against a deterministic argument so the
     * libm fallback can branch-predict; the bench is measuring the
     * SDK seam, not libm's own performance. */
    BENCH_RUN("alp_tmu_sin", 1000000, {
        float out;
        (void)alp_tmu_sin(0.5f, &out);
    });

    BENCH_RUN("alp_tmu_cos", 1000000, {
        float out;
        (void)alp_tmu_cos(0.5f, &out);
    });

    BENCH_RUN("alp_tmu_tan", 1000000, {
        float out;
        (void)alp_tmu_tan(0.5f, &out);
    });

    BENCH_RUN("alp_tmu_atan2", 1000000, {
        float out;
        (void)alp_tmu_atan2(1.0f, 1.0f, &out);
    });

    BENCH_RUN("alp_tmu_sqrt", 1000000, {
        float out;
        (void)alp_tmu_sqrt(4.0f, &out);
    });

    BENCH_RUN("alp_tmu_log", 1000000, {
        float out;
        (void)alp_tmu_log(1.5f, &out);
    });

    BENCH_RUN("alp_tmu_exp", 1000000, {
        float out;
        (void)alp_tmu_exp(0.5f, &out);
    });

    BENCH_RUN("alp_tmu_hypot", 1000000, {
        float out;
        (void)alp_tmu_hypot(3.0f, 4.0f, &out);
    });

    /* NULL-out rejection -- representative INVAL exit cost across
     * every primitive. */
    BENCH_RUN("alp_tmu_sin(NULL out)", 1000000,
              { (void)alp_tmu_sin(0.5f, NULL); });
}
