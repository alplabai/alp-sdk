/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB device + host NOSUPPORT stubs -- <alp/usb.h>.  Split out of
 * the former src/common/stub_backend.c monolith (issue #673).
 * Unguarded: no vendor backend has ever overridden this class.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/usb.h"

alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_usb_device_enable(alp_usb_dev_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_disable(alp_usb_dev_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_write(alp_usb_dev_t *d, const uint8_t *b, size_t l, uint32_t t)
{
	(void)d;
	(void)b;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_read(alp_usb_dev_t *d, uint8_t *b, size_t l, size_t *o, uint32_t t)
{
	(void)d;
	(void)b;
	(void)l;
	(void)t;
	if (o) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_usb_device_close(alp_usb_dev_t *d)
{
	(void)d;
}
const alp_capabilities_t *alp_usb_capabilities(const alp_usb_dev_t *dev)
{
	(void)dev;
	return NULL;
}
alp_usb_host_t *alp_usb_host_open(void)
{
	return NULL;
}
alp_status_t alp_usb_host_enable(alp_usb_host_t *h)
{
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_host_disable(alp_usb_host_t *h)
{
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
void alp_usb_host_close(alp_usb_host_t *h)
{
	(void)h;
}
