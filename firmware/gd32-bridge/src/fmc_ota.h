/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — flash HW seam.
 *
 * Real implementation in hal/fmc_ota.c (gd32 backend; the erase/program
 * inner loop runs from RAM so the FMC stall doesn't fault the executing
 * core).  Weak no-op defaults live in ota.c so the stub backend links
 * without the vendor FMC driver — there ota_fmc_supported() returns false
 * and the OTA state machine degrades to STATUS_NOSUPPORT.
 */
#ifndef GD32_BRIDGE_FMC_OTA_H
#define GD32_BRIDGE_FMC_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* false on a build without real FMC support (stub backend) -> OTA inert. */
bool ota_fmc_supported(void);

/* Erase [base, base+len): base + len must be page-aligned (OTA_PAGE_SIZE).
 * Returns false on FMC error. */
bool ota_fmc_erase_range(uint32_t base, uint32_t len);

/* Program `len` bytes at `addr` (flash). `addr` and `len` honour the
 * device's program granularity (handled inside). Returns false on error. */
bool ota_fmc_program(uint32_t addr, const uint8_t *data, size_t len);

#endif /* GD32_BRIDGE_FMC_OTA_H */
