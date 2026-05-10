/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thread-local last-error storage for `alp_*_open` failure
 * diagnosis.  The public read accessor is `alp_last_error`; the
 * internal write accessor lives in handles.h so per-peripheral
 * source files can stamp a precise reason before returning NULL.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "handles.h"

/* Per-thread error code.  Zephyr's __thread keyword maps to its
 * thread-local-storage facility (CONFIG_THREAD_LOCAL_STORAGE).  When
 * TLS is unavailable (e.g. minimal libc on M-class targets without a
 * TLS area), fall back to a single global — the cost is that
 * concurrent open() calls on multiple threads can clobber each
 * other's last-error slot, which is rare and never a safety issue. */
#if defined(CONFIG_THREAD_LOCAL_STORAGE)
static __thread alp_status_t z_last_err;
#else
static alp_status_t z_last_err;
#endif

void alp_z_set_last_error(alp_status_t s) {
    z_last_err = s;
}

void alp_z_clear_last_error(void) {
    z_last_err = ALP_OK;
}

alp_status_t alp_last_error(void) {
    return z_last_err;
}
