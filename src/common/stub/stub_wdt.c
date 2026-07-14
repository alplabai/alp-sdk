/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watchdog NOSUPPORT stubs -- <alp/wdt.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_wdt_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/wdt.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_WDT)
alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_wdt_feed(alp_wdt_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_wdt_disable(alp_wdt_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
void alp_wdt_close(alp_wdt_t *w)
{
	(void)w;
}
const alp_capabilities_t *alp_wdt_capabilities(const alp_wdt_t *w)
{
	(void)w;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_WDT */
