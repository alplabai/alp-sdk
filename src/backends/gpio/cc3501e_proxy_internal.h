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

/* Test-only hook: arm a canned alp_hw_info_read() result for the NEXT
 * alp_gpio_cc3501e_attach() call to consume instead of a real I2C
 * transaction -- unlike the force hook above (which sets the cached
 * decision directly, bypassing attach() entirely), this exercises
 * attach()'s REAL body: the real cc3501e_proxy_hw_rev_confirmed_match()
 * call and the real assignment to the cached decision, with only the
 * alp_hw_info_read() source swapped out.  native_sim has no I2C EEPROM to
 * back a real read (see cc3501e_proxy.c), so this is the seam a positive
 * "attach() confirms a match" test uses.  One-shot: attach() disarms it
 * after reading, so a later attach() call without re-arming falls back to
 * the real alp_hw_info_read(). Never called by production code. */
void cc3501e_proxy_test_inject_hw_info_read(alp_status_t status, const alp_hw_info_t *info);

/* Test-only teardown: reset the cached bridge context pointer to NULL.
 * A test proving alp_gpio_cc3501e_attach() itself (real body, not the
 * force hook) has no real cc3501e_t to pass it and uses a deliberately
 * never-dereferenced non-NULL sentinel instead (see cc3501e_proxy.c) --
 * call this after such a test so the sentinel does not outlive it and
 * later tests cannot dereference it via a route that turns is_bridge
 * true. Never called by production code. */
void cc3501e_proxy_test_reset_bridge_ctx(void);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_SRC_BACKENDS_GPIO_CC3501E_PROXY_INTERNAL_H */
