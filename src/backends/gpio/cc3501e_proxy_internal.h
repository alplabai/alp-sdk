/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Alp Lab AB
 *
 * Internal test-visibility surface for the CC3501E GPIO proxy backend
 * (src/backends/gpio/cc3501e_proxy.c) -- NOT part of the public <alp/...>
 * API.  Split out (issue #2144 review) so a test source declaring these
 * two symbols by hand can never silently drift from the .c file's own
 * definitions -- a hand-copied `extern` with a mismatched signature would
 * previously compile clean and only fail (or worse, mis-link) far from the
 * mistake.
 */

#ifndef ALP_SRC_BACKENDS_GPIO_CC3501E_PROXY_INTERNAL_H
#define ALP_SRC_BACKENDS_GPIO_CC3501E_PROXY_INTERNAL_H

#include <stdbool.h>

#include <alp/hw_info.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_ALP_SDK_HW_INFO)
/* Pure decision, no I2C: does @p read_status + @p info -- an
 * alp_hw_info_read() result -- confirm the running module's hw_rev matches
 * CONFIG_ALP_SDK_SOM_HW_REV?  See cc3501e_proxy.c for the full rationale. */
bool cc3501e_proxy_hw_rev_confirmed_match(alp_status_t read_status, const alp_hw_info_t *info);
#endif

#if defined(CONFIG_ZTEST)
/* Test-only hook: force the cached decision without a real EEPROM read.
 * Never called by production code -- see cc3501e_proxy.c. */
void cc3501e_proxy_test_force_hw_rev_confirmed_match(bool confirmed);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_SRC_BACKENDS_GPIO_CC3501E_PROXY_INTERNAL_H */
