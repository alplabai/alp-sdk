/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power NOSUPPORT stubs -- <alp/power.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_power_*` symbol not provided by a vendor backend.
 */

#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/power.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_POWER)
alp_power_t *alp_power_open(void)
{
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_power_configure_wake_source(alp_power_t *p, uint32_t wake_bitmap)
{
	(void)p;
	(void)wake_bitmap;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_power_request_sleep(alp_power_t           *p,
                                     alp_power_mode_t       mode,
                                     uint32_t               wake_after_ms,
                                     alp_power_wake_info_t *info)
{
	(void)p;
	(void)mode;
	(void)wake_after_ms;
	(void)info;
	return ALP_ERR_NOSUPPORT;
}
void alp_power_close(alp_power_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_POWER */
