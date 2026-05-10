/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Top-level driver for the ALP SDK microbench suite.  Each
 * `bench_<api>_main()` function in this directory contributes
 * one or more BENCH_RUN cases; this file just chains them so the
 * suite runs as a single binary that prints all cases in one pass.
 *
 * v0.3: three suites (peripheral, inference, status).  v1.0
 * extends per-API as implementations land.
 */

#include <stdio.h>

void bench_peripheral_main(void);
void bench_inference_main(void);
void bench_status_main(void);

int  main(void)
{
    fprintf(stdout, "# alp_bench -- ALP SDK microbenchmarks\n");
    fprintf(stdout, "# %-38s %12s %14s\n", "case", "iters", "ns/iter");

    bench_status_main();
    bench_peripheral_main();
    bench_inference_main();

    return 0;
}
