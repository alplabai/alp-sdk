/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Top-level driver for the Alp SDK microbench suite.  Each
 * `bench_<api>_main()` function in this directory contributes
 * one or more BENCH_RUN cases; this file just chains them so the
 * suite runs as a single binary that prints all cases in one pass.
 *
 * v0.3: six suites (status, peripheral, inference, iot, audio,
 * storage).  §C.19 adds four more (dsp, tmu, power, security)
 * tracking the v0.5 wave-2 surfaces.  v1.0 extends per-API as
 * implementations land.
 */

#include <stdio.h>

void bench_peripheral_main(void);
void bench_inference_main(void);
void bench_status_main(void);
void bench_iot_main(void);
void bench_audio_main(void);
void bench_storage_main(void);
void bench_dsp_main(void);
void bench_tmu_main(void);
void bench_power_main(void);
void bench_security_main(void);

int  main(void)
{
    fprintf(stdout, "# alp_bench -- Alp SDK microbenchmarks\n");
    fprintf(stdout, "# %-38s %12s %14s\n", "case", "iters", "ns/iter");

    bench_status_main();
    bench_peripheral_main();
    bench_inference_main();
    bench_iot_main();
    bench_audio_main();
    bench_storage_main();
    bench_dsp_main();
    bench_tmu_main();
    bench_power_main();
    bench_security_main();

    return 0;
}
