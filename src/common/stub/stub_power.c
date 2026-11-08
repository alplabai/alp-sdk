/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power NOSUPPORT stubs -- <alp/power.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_power_*` symbol not provided by a vendor backend.
 */

#include <stdint.h>
#include <string.h>

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
alp_status_t alp_power_configure_retention(alp_power_t *p, const alp_power_retain_t *retain)
{
	(void)p;
	(void)retain;
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
alp_status_t alp_power_domain_policy_set(alp_power_t              *p,
                                         alp_power_domain_t        domain,
                                         alp_power_domain_policy_t policy)
{
	(void)p;
	(void)domain;
	(void)policy;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_power_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out)
{
	(void)domain;
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_power_boot_wake_info(alp_power_boot_info_t *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	return ALP_ERR_NOSUPPORT;
}
#endif /* !ALP_VENDOR_OVERRIDES_POWER */
