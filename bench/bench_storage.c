/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/storage.h>.  Storage opens on the stub
 * backend return NULL fast -- useful baseline for apps that probe
 * for an SD-card / eMMC / on-flash region at boot.  v1.0 adds
 * real-stack benches (random / sequential read+write at 4K / 64K /
 * 1M block sizes, sync barriers) on the LittleFS path.
 */

#include "bench.h"

#include "alp/storage.h"

void bench_storage_main(void)
{
    BENCH_RUN("alp_storage_open(NULL)", 1000000, { (void)alp_storage_open(NULL); });

    /* alp_storage_get_info on a NULL handle stamps the info struct
     * and returns NOSUPPORT.  Cheap. */
    BENCH_RUN("alp_storage_get_info(NULL)", 1000000, {
        alp_storage_info_t info;
        (void)alp_storage_get_info(NULL, &info);
    });

    /* Round-trip the four read/write/erase/sync accessors with
     * NULL handles -- the NULL-handle guards every wrapper
     * function carries on the stub path. */
    BENCH_RUN("alp_storage_read(NULL)", 1000000, { (void)alp_storage_read(NULL, 0u, NULL, 0u); });
    BENCH_RUN("alp_storage_write(NULL)", 1000000, { (void)alp_storage_write(NULL, 0u, NULL, 0u); });
    BENCH_RUN("alp_storage_erase(NULL)", 1000000, { (void)alp_storage_erase(NULL, 0u, 0u); });
    BENCH_RUN("alp_storage_sync(NULL)", 1000000, { (void)alp_storage_sync(NULL); });
}
