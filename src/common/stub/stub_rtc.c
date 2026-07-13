/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTC NOSUPPORT stubs -- <alp/rtc.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_rtc_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/rtc.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_RTC)
alp_rtc_t *alp_rtc_open(uint32_t rtc_id)
{
	(void)rtc_id;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_rtc_set_time(alp_rtc_t *r, const alp_rtc_time_t *t)
{
	(void)r;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_rtc_get_time(alp_rtc_t *r, alp_rtc_time_t *t)
{
	(void)r;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_rtc_close(alp_rtc_t *r)
{
	(void)r;
}
const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *r)
{
	(void)r;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_RTC */
