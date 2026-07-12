/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE (+ GATT) NOSUPPORT stubs -- <alp/ble.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_ble_*` symbol not provided by a vendor backend.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/ble.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_BLE)
alp_ble_t *alp_ble_open(void)
{
	return NULL;
}
void alp_ble_close(alp_ble_t *b)
{
	(void)b;
}
alp_status_t alp_ble_advertise_start(alp_ble_t *b, const alp_ble_adv_config_t *c)
{
	(void)b;
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_advertise_stop(alp_ble_t *b)
{
	(void)b;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_register_service(alp_ble_t                   *b,
                                           const alp_ble_service_def_t *d,
                                           alp_ble_attr_handle_t       *h)
{
	(void)b;
	(void)d;
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_notify(alp_ble_t            *b,
                                 alp_ble_conn_t       *c,
                                 alp_ble_attr_handle_t h,
                                 const uint8_t        *p,
                                 size_t                l)
{
	(void)b;
	(void)c;
	(void)h;
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_scan_start(alp_ble_t *b, bool a, alp_ble_scan_cb_t cb, void *u)
{
	(void)b;
	(void)a;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_scan_stop(alp_ble_t *b)
{
	(void)b;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_ble_connect(alp_ble_t *b, const alp_ble_addr_t *p, uint32_t t, alp_ble_conn_t **out)
{
	(void)b;
	(void)p;
	(void)t;
	if (out) *out = NULL;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_disconnect(alp_ble_conn_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_read(alp_ble_conn_t       *c,
                               alp_ble_attr_handle_t h,
                               uint8_t              *o,
                               size_t                cap,
                               size_t               *ol,
                               uint32_t              t)
{
	(void)c;
	(void)h;
	(void)o;
	(void)cap;
	(void)ol;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_write(alp_ble_conn_t       *c,
                                alp_ble_attr_handle_t h,
                                const uint8_t        *d,
                                size_t                l,
                                uint32_t              t)
{
	(void)c;
	(void)h;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
const alp_capabilities_t *alp_ble_capabilities(const alp_ble_t *ble)
{
	(void)ble;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_BLE */
