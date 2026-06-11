/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the update-log entry/meta decode path -- the code
 * that ingests attacker-reachable bytes from persistent storage. Links the
 * REAL engine (engine.c + sha256.c) so it fuzzes production decode, not a
 * mirror.
 *
 * Build:  cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal -DCMAKE_C_COMPILER=clang
 *         cmake --build build-fuzz --target alp_fuzz_update_log_entry
 * Run:    ./build-fuzz/tests/fuzz/alp_fuzz_update_log_entry -max_total_time=30 \
 *               tests/fuzz/corpus/update_log_entry
 */
#include <stddef.h>
#include <stdint.h>

#include "update_log/engine.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    alp_update_log_entry_t e;
    uint8_t                prev[32];
    (void)ulog_entry_decode(data, size, &e, prev);

    struct ulog_meta m;
    (void)ulog_meta_decode(data, size, &m);
    return 0;
}
