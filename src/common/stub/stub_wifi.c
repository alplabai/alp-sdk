/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi NOSUPPORT stubs -- <alp/iot.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_wifi_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/iot.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_WIFI)
alp_wifi_t *alp_wifi_open(void)
{
	return NULL;
}
alp_status_t alp_wifi_connect(alp_wifi_t *w, const alp_wifi_credentials_t *c, uint32_t t)
{
	(void)w;
	(void)c;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_wifi_disconnect(alp_wifi_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
void alp_wifi_close(alp_wifi_t *w)
{
	(void)w;
}
const alp_capabilities_t *alp_wifi_capabilities(const alp_wifi_t *w)
{
	(void)w;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_WIFI */
