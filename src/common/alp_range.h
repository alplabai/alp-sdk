/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Overflow-safe range-containment check shared by every offset+length
 * bounds check in the SDK (storage read/write/erase, the .alpmodel
 * manifest/blob-table decoder, the RPC frame builders).
 *
 * A naive `off + len > size` compare wraps: an `off`/`len` pair near
 * UINT64_MAX (or a 32-bit size_t sum near SIZE_MAX) makes `off + len`
 * overflow and land below `size`, so the compare passes and the
 * out-of-range access proceeds instead of being rejected. This header
 * checks containment without ever forming the `off + len` sum.
 *
 * Lives in src/common/ (not include/alp/) -- internal to the SDK, not
 * part of the customer-facing <alp/...> surface. Reachable from every
 * OS backend: src/common is on the include path for all three ALP_OS
 * variants (src/common/CMakeLists.txt) and is added explicitly for
 * the Zephyr module build (zephyr/CMakeLists.txt).
 */

#ifndef ALP_RANGE_H_
#define ALP_RANGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Overflow-safe "does [off, off+len) fit inside [0, size)" check.
 *
 * @param off  Range start.
 * @param len  Range length.
 * @param size Container size (exclusive upper bound).
 * @return true if the whole [off, off+len) range fits inside
 *         [0, size); false otherwise -- including when off+len would
 *         overflow. Never computes `off + len`.
 */
static inline bool alp_range_ok(uint64_t off, uint64_t len, uint64_t size)
{
	return len <= size && off <= size - len;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_RANGE_H_ */
