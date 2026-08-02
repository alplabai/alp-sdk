/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Overflow-safe [offset, offset+len) bounds check for the vendored MRAM
 * flash driver (see flash_mram_alif.c for the full ADR 0017 provenance
 * banner and the #1119 divergence note).  Split into its own tiny,
 * dependency-free header (off_t/size_t plus the shared
 * src/common/alp_checked_arith.h helper) so tests/unit/flash_mram_range can
 * exercise the exact check the driver runs, without pulling in
 * DEVICE_MMIO/cmsis_core/devicetree.
 */
#ifndef ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_
#define ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_

#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>

#include "alp_checked_arith.h"

/**
 * @brief Check whether [offset, offset+len) fits inside [0, size).
 *
 * `offset` is a signed off_t (the flash-driver API's native type); reject
 * negative offsets outright, then delegate the overflow-safe subtraction
 * check to the shared alp_size_range_valid() helper (src/common/
 * alp_checked_arith.h, #743) -- it never computes `offset + len`, so a
 * near-SIZE_MAX `len` cannot wrap the sum back under `size` and evade the
 * bound (#1119).
 *
 * @param offset Start offset of the requested range.
 * @param len Length of the requested range, in bytes.
 * @param size Size of the valid region, in bytes.
 *
 * @return true if the range is fully inside [0, size), false otherwise.
 */
static inline bool flash_mram_range_is_valid(off_t offset, size_t len, size_t size)
{
	if (offset < 0) {
		return false;
	}

	return alp_size_range_valid((size_t)offset, len, size);
}

#endif /* ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_ */
