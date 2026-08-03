/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Overflow-safe [offset, offset+len) bounds check for the vendored MRAM
 * flash driver (see flash_mram_alif.c for the full ADR 0017 provenance
 * banner and the #1119 divergence note).  Split into its own tiny,
 * dependency-free header (off_t/size_t only) so tests/unit/flash_mram_range
 * can exercise the exact check the driver runs, without pulling in
 * DEVICE_MMIO/cmsis_core/devicetree.
 *
 * Deliberately does NOT include src/common/alp_checked_arith.h: this driver
 * (flash_mram_alif.c) is compiled BEFORE zephyr/CMakeLists.txt's
 * `if(NOT CONFIG_ALP_SDK) return()` early-return, for images (e.g. MCUboot,
 * zephyr/Kconfig:52-60) that never set CONFIG_ALP_SDK -- and the
 * `zephyr_include_directories(src/common)` call is only reached AFTER that
 * return, so `alp_checked_arith.h` is unresolvable in exactly those images.
 * The subtraction check is duplicated inline (four lines) rather than
 * relying on the shared helper; if `alp_size_range_valid()` ever changes,
 * update both.
 */
#ifndef ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_
#define ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_

#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>

/**
 * @brief Check whether [offset, offset+len) fits inside [0, size).
 *
 * `offset` is a signed off_t (the flash-driver API's native type); reject
 * negative offsets outright.  The subtraction-based bound check below never
 * computes `offset + len` -- that sum can wrap before a naive
 * `offset + len > size` comparison runs -- so a near-SIZE_MAX `len` cannot
 * wrap the sum back under `size` and evade the bound (#1119).  This mirrors
 * src/common/alp_checked_arith.h's alp_size_range_valid(), duplicated here
 * rather than included -- see the file banner above.
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

	if ((size_t)offset > size) {
		return false;
	}

	/* offset <= size here, so size - offset cannot wrap. */
	return len <= size - (size_t)offset;
}

#endif /* ZEPHYR_DRIVERS_FLASH_FLASH_MRAM_RANGE_H_ */
