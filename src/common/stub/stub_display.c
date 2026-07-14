/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display NOSUPPORT stubs -- <alp/display.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_display_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>

#include "alp/display.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_DISPLAY)
alp_display_t *alp_display_open(const alp_display_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_display_clear(alp_display_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_display_print(alp_display_t *d, const char *s)
{
	(void)d;
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
void alp_display_close(alp_display_t *d)
{
	(void)d;
}
#endif /* !ALP_VENDOR_OVERRIDES_DISPLAY */
