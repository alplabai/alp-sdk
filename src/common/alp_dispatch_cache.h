/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Publication helper for the handle-less "stateless dispatcher" ops
 * caches (issue #628): TMU, SoC-info, peer-core boot, power-profile,
 * and the security random-bytes fast path all lazily resolve their
 * backend's ops vtable on first call and cache the pointer so every
 * later call skips alp_backend_select().  The cache used to be a
 * plain `static const T *_cached_ops` read/written with no
 * synchronization -- concurrent first calls raced a plain read
 * against a plain write, which is a C data race even though every
 * writer computes the identical pointer (alp_backend_select() is a
 * deterministic, immutable-after-link lookup, so the race is benign
 * in VALUE terms but not in the C memory model).
 *
 * These two helpers make the read an acquire-load and the write a
 * release-store through the compiler's GCC/Clang atomic builtins --
 * same portable, no-OS-mutex-needed mechanism as
 * src/common/alp_slot_claim.h, so every dispatcher TU (Zephyr,
 * baremetal, yocto) gets the same fix with no ALP_OS branching.  The
 * hot path after the first call stays exactly what it was: one load
 * + one branch + one indirect call -- the acquire load compiles to a
 * plain load on every ALP_OS target we ship (x86/ARM64 need no
 * fence for a load-acquire; ARMv7-M/M33/M55 folds it into the
 * existing ldr), so there is no measured steady-state regression.
 */

#ifndef ALP_COMMON_DISPATCH_CACHE_H
#define ALP_COMMON_DISPATCH_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire-load a lazily-published, idempotent dispatch cache.
 *
 * @param[in] cache  Address of the `static const T *_cached_ops` slot.
 * @return The cached ops pointer, or NULL if nobody has published yet.
 */
static inline const void *alp_dispatch_cache_load(const void *const *cache)
{
	return __atomic_load_n(cache, __ATOMIC_ACQUIRE);
}

/**
 * @brief Release-store a resolved ops pointer into a dispatch cache.
 *
 * Safe to call from multiple racing first-callers: every caller
 * resolves the same backend (the registry is immutable after link),
 * so a "duplicate" publish just rewrites the identical pointer value
 * -- idempotent, no invalidation logic needed.
 *
 * @param[in,out] cache  Address of the `static const T *_cached_ops` slot.
 * @param[in]     value  Resolved ops pointer to publish.
 */
static inline void alp_dispatch_cache_store(const void **cache, const void *value)
{
	__atomic_store_n(cache, value, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* ALP_COMMON_DISPATCH_CACHE_H */
