/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <soc_memory_map.h> (see se_service.h here).
 */
#ifndef ALP_TEST_FAKE_SOC_MEMORY_MAP_H
#define ALP_TEST_FAKE_SOC_MEMORY_MAP_H

#include <stdint.h>

/* Identity translation is enough: the test compares buffer addresses. */
#define local_to_global(p) ((uint32_t)(uintptr_t)(p))

#endif
