/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-backend internal helpers for the Alp SDK.  Not part of the
 * public surface -- application code must use the <alp/...> public
 * headers instead.
 *
 * Lives in src/common/ so every backend (baremetal, yocto, and any
 * non-Zephyr build that picks up stub_backend.c) shares one
 * canonical implementation.  Zephyr has its own thread-local
 * last-error in src/zephyr/last_error.c; this header is irrelevant
 * to Zephyr builds.
 */

#ifndef ALP_INTERNAL_H_
#define ALP_INTERNAL_H_

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread-local storage qualifier for the plain-CMake
 *        last-error slot (`src/common/stub_backend.c`'s
 *        `z_last_error`).
 *
 * `alp_last_error()`'s public contract (<alp/peripheral.h>) is
 * per-thread: a failure recorded on one thread must never be read
 * back -- or clobbered -- by another.  On a hosted Linux target
 * (the Yocto backend, and the plain-CMake "baremetal" build that,
 * absent a vendor cross-toolchain file, also compiles and runs
 * natively on the CI host -- see `stub_backend.c`'s `__linux__`-gated
 * delay primitives for the same split) glibc's C11 `_Thread_local`
 * gives every pthread its own slot.  A genuine non-Linux bare-metal
 * target has no threading model beneath this layer at all (one
 * execution context, no RTOS), so a plain static is already
 * thread-safe by construction -- and TLS runtime support may not
 * even be linkable there (no thread-pointer setup in a from-scratch
 * startup file) -- so this degrades to no qualifier rather than
 * forcing one.
 */
#if defined(__linux__)
#define ALP_LAST_ERROR_TLS _Thread_local
#else
#define ALP_LAST_ERROR_TLS
#endif

/**
 * @brief Stamp the thread-local last-error slot read by
 *        `alp_last_error`.
 *
 * Use from any backend source file that needs to convey a precise
 * failure reason before returning NULL or a non-OK status.  The
 * setter writes to the same `ALP_LAST_ERROR_TLS`-qualified static
 * that `src/common/stub_backend.c`'s peripheral stubs write to, so
 * a caller invoking `alp_last_error()` after a failure sees the
 * latest write regardless of which layer stamped it -- including
 * `ALP_VENDOR_OVERRIDES_PERIPHERAL=1` builds (vendor wrappers such
 * as `vendors/alif/`, `vendors/renesas-rzv2n/`): those wrappers call
 * this same setter rather than owning a separate static and a
 * duplicate `alp_last_error` reader, so there is exactly one
 * last-error slot and one canonical public accessor regardless of
 * which layer stamped the failure.
 *
 * A successful `alp_*_open()` MUST call this with @ref ALP_OK so the
 * calling thread's slot doesn't carry a stale failure forward.
 */
void alp_internal_set_last_error(alp_status_t s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_INTERNAL_H_ */
