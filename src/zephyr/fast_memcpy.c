/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * memcpy() / __aeabi_memcpy*() override for -Os Ensemble images
 * (CONFIG_ALP_SDK_FAST_MEMCPY, issue #2285). See
 * src/common/alp_fast_memcpy_core.c's file header for the why and the
 * self-recursion trap this depends on the core function avoiding.
 *
 * Just thin aliases onto the shared, independently-testable core
 * (src/common/alp_fast_memcpy_core.c) -- kept out of that file so the
 * host unit test (tests/unit/fast_memcpy) can link the core without
 * also defining memcpy() itself, which would collide with native_sim's
 * own host libc.
 *
 * No multiple-definition risk against picolibc's own memcpy: this TU's
 * object file is linked into the alp_sdk zephyr_library ahead of the
 * picolibc archive, so by the time the linker reaches picolibc's
 * archive member defining memcpy, the symbol is already resolved and
 * that member is never pulled in.
 */

#include "alp_fast_memcpy_core.h"

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
	return alp_fast_memcpy_core(dst, src, n);
}

/* GCC recognises "memcpy" as a builtin by name and grants it implicit
 * leaf/nonnull/nothrow attributes regardless of this TU's own
 * declaration; an alias target that doesn't repeat them trips
 * -Wmissing-attributes even though the aliased bodies are identical.
 * Silence just that one diagnostic around the three aliases rather
 * than guess at (and pin ourselves to) GCC's exact implicit set. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-attributes"
void *__aeabi_memcpy(void *dst, const void *src, size_t n) __attribute__((alias("memcpy")));
void *__aeabi_memcpy4(void *dst, const void *src, size_t n) __attribute__((alias("memcpy")));
void *__aeabi_memcpy8(void *dst, const void *src, size_t n) __attribute__((alias("memcpy")));
#pragma GCC diagnostic pop
