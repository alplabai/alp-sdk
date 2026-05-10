/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tiny header-only microbench harness for the ALP SDK.  See
 * bench/README.md for the rationale.  No external dependencies --
 * timing comes from clock_gettime on POSIX, k_uptime_ticks on Zephyr.
 *
 * Usage:
 *   BENCH_RUN("alp_foo_open", 1000000, {
 *       (void)alp_foo_open(NULL);
 *   });
 */

#ifndef ALP_BENCH_H_
#define ALP_BENCH_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
static inline uint64_t alp_bench_now_ns(void)
{
    return k_ticks_to_ns_floor64(k_uptime_ticks());
}
#else
#include <time.h>
static inline uint64_t alp_bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/**
 * @brief Run @p body @p iters times and print ns/iter to stdout.
 *
 * The harness compiles with -O2; the @p body block is a do-block so
 * locals don't leak and the loop body is inlinable.  Callers usually
 * pass enough iters to amortise the clock overhead (1e5 - 1e7 for ns
 * scale, 1e3 - 1e4 for us scale).
 */
#define BENCH_RUN(name_, iters_, body_)                                                            \
    do {                                                                                           \
        const size_t   _alp_bench_iters = (size_t)(iters_);                                        \
        const uint64_t _alp_bench_t0    = alp_bench_now_ns();                                      \
        for (size_t _alp_bench_i = 0; _alp_bench_i < _alp_bench_iters; ++_alp_bench_i) {           \
            body_                                                                                  \
        }                                                                                          \
        const uint64_t _alp_bench_t1 = alp_bench_now_ns();                                         \
        const uint64_t _alp_bench_ns =                                                             \
            (_alp_bench_iters == 0)                                                                \
                ? 0                                                                                \
                : ((_alp_bench_t1 - _alp_bench_t0) / (uint64_t)_alp_bench_iters);                  \
        fprintf(stdout, "%-40s %12lu iters %12lu ns/iter\n", (name_),                              \
                (unsigned long)_alp_bench_iters, (unsigned long)_alp_bench_ns);                    \
    } while (0)

#endif /* ALP_BENCH_H_ */
