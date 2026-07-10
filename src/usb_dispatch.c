/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB class dispatcher.  Owns both halves of the public <alp/usb.h>
 * surface (device + host) and routes through the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers both
 * surfaces, since they share the underlying USB controller.  The
 * ops vtable carries function pointers for both surfaces; the
 * dispatcher maintains two separate handle pools.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/usb.h>

#include "backends/usb/usb_ops.h"

ALP_BACKEND_DEFINE_CLASS(usb);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES
#define CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_USB_HOST_HANDLES
#define CONFIG_ALP_SDK_MAX_USB_HOST_HANDLES 1
#endif

static struct alp_usb_dev  _dev_pool[CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES];
static struct alp_usb_host _host_pool[CONFIG_ALP_SDK_MAX_USB_HOST_HANDLES];

static struct alp_usb_dev *_alloc_dev(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES; ++i) {
		if (!_dev_pool[i].in_use) {
			memset(&_dev_pool[i], 0, sizeof(_dev_pool[i]));
			_dev_pool[i].in_use = true;
			return &_dev_pool[i];
		}
	}
	return NULL;
}

static void _free_dev(struct alp_usb_dev *h)
{
	h->in_use = false;
}

static struct alp_usb_host *_alloc_host(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_USB_HOST_HANDLES; ++i) {
		if (!_host_pool[i].in_use) {
			memset(&_host_pool[i], 0, sizeof(_host_pool[i]));
			_host_pool[i].in_use = true;
			return &_host_pool[i];
		}
	}
	return NULL;
}

static void _free_host(struct alp_usb_host *h)
{
	h->in_use = false;
}

/* ================================================================== */
/* Device-side dispatch                                                */
/* ================================================================== */

alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("usb", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_usb_ops_t *ops = (const alp_usb_ops_t *)be->ops;
	if (ops == NULL || ops->dev_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_usb_dev *h = _alloc_dev();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.cfg            = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->dev_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_dev(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_usb_device_enable(alp_usb_dev_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->dev_enable == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->dev_enable(&h->state);
}

alp_status_t alp_usb_device_disable(alp_usb_dev_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->dev_disable == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->dev_disable(&h->state);
}

alp_status_t
alp_usb_device_write(alp_usb_dev_t *h, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (h->state.ops == NULL || h->state.ops->dev_write == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->dev_write(&h->state, data, len, timeout_ms);
}

alp_status_t alp_usb_device_read(alp_usb_dev_t *h,
                                 uint8_t       *data,
                                 size_t         len,
                                 size_t        *out_len,
                                 uint32_t       timeout_ms)
{
	if (out_len != NULL) *out_len = 0;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (h->state.ops == NULL || h->state.ops->dev_read == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->dev_read(&h->state, data, len, out_len, timeout_ms);
}

void alp_usb_device_close(alp_usb_dev_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->dev_close != NULL) {
		h->state.ops->dev_close(&h->state);
	}
	_free_dev(h);
}

/* ================================================================== */
/* Host-side dispatch                                                  */
/* ================================================================== */

alp_usb_host_t *alp_usb_host_open(void)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("usb", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_usb_ops_t *ops = (const alp_usb_ops_t *)be->ops;
	if (ops == NULL || ops->host_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_usb_host *h = _alloc_host();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->host_open(&h->state, &caps);
	if (rc != ALP_OK) {
		_free_host(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_usb_host_enable(alp_usb_host_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->host_enable == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->host_enable(&h->state);
}

alp_status_t alp_usb_host_disable(alp_usb_host_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	if (h->state.ops == NULL || h->state.ops->host_disable == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return h->state.ops->host_disable(&h->state);
}

void alp_usb_host_close(alp_usb_host_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->host_close != NULL) {
		h->state.ops->host_close(&h->state);
	}
	_free_host(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_usb_capabilities(const alp_usb_dev_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
