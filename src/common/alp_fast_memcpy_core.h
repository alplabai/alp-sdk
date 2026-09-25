/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Word-copy memcpy core, shared between the Zephyr memcpy override
 * (src/zephyr/fast_memcpy.c, CONFIG_ALP_SDK_FAST_MEMCPY) and the
 * native_sim host unit test (tests/unit/fast_memcpy). See
 * alp_fast_memcpy_core.c's file header for why this exists and the
 * self-recursion trap it works around -- kept out of this header so the
 * trap's explanation lives next to the code it explains.
 */

#ifndef ALP_COMMON_FAST_MEMCPY_CORE_H
#define ALP_COMMON_FAST_MEMCPY_CORE_H

#include <stddef.h>

/**
 * @brief Copy @p n bytes from @p src to @p dst, word-at-a-time where alignment allows.
 *
 * Same contract as C11 memcpy(): the regions must not overlap (use
 * memmove() semantics if they might -- this function does not provide
 * them). Safe to call with @p n == 0 and non-null @p dst / @p src.
 *
 * @param[out] dst  Destination buffer, at least @p n bytes, must not overlap @p src.
 * @param[in]  src  Source buffer, at least @p n bytes, must not overlap @p dst.
 * @param[in]  n    Number of bytes to copy.
 * @return @p dst.
 */
void *alp_fast_memcpy_core(void *restrict dst, const void *restrict src, size_t n);

#endif /* ALP_COMMON_FAST_MEMCPY_CORE_H */
