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

#include "alp_slot_claim.h"
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
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_dev_pool[i].in_use)) {
			memset(&_dev_pool[i], 0, offsetof(struct alp_usb_dev, in_use));
			return &_dev_pool[i];
		}
	}
	return NULL;
}

static void _free_dev(struct alp_usb_dev *h)
{
	alp_slot_release(&h->in_use);
}

static struct alp_usb_host *_alloc_host(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_USB_HOST_HANDLES; ++i) {
		if (alp_slot_try_claim(&_host_pool[i].in_use)) {
			memset(&_host_pool[i], 0, offsetof(struct alp_usb_host, in_use));
			return &_host_pool[i];
		}
	}
	return NULL;
}

static void _free_host(struct alp_usb_host *h)
{
	alp_slot_release(&h->in_use);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_usb_device_enable(alp_usb_dev_t *h)
{
	/* Gate on the lifecycle byte, not a plain in_use read -- see
	 * alp_slot_claim.h's op_enter/leave doc comment (issue #629). */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->dev_enable == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->dev_enable(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_usb_device_disable(alp_usb_dev_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->dev_disable == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->dev_disable(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t
alp_usb_device_write(alp_usb_dev_t *h, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (data == NULL && len > 0) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629): write() can
	 * block up to timeout_ms draining the transfer, so
	 * alp_usb_device_close() drains this op with the sleep-poll
	 * alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c)
	 * instead of the busy-spin alp_handle_begin_close() -- generalised
	 * from rpc_dispatch.c's _rpc_op_enter()/_rpc_begin_close()/
	 * _rpc_drain() (GHSA-xhm8). */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->dev_write == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->dev_write(&h->state, data, len, timeout_ms);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_usb_device_read(alp_usb_dev_t *h,
                                 uint8_t       *data,
                                 size_t         len,
                                 size_t        *out_len,
                                 uint32_t       timeout_ms)
{
	if (out_len != NULL) *out_len = 0;
	if (data == NULL && len > 0) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629) -- see
	 * alp_usb_device_write() above for the same rationale. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->dev_read == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->dev_read(&h->state, data, len, out_len, timeout_ms);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_usb_device_close(alp_usb_dev_t *h)
{
	if (h == NULL) return;
	/* Sleep-poll drain (issue #629): this pool counts
	 * alp_usb_device_read()/alp_usb_device_write(), each of which can
	 * block up to its caller's timeout_ms, so
	 * alp_handle_begin_close_blocking() sleeps between polls instead of
	 * busy-spinning -- see src/common/alp_slot_claim.c/.h. Idempotent: a
	 * second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) return;
	if (h->state.ops != NULL && h->state.ops->dev_close != NULL) {
		h->state.ops->dev_close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_usb_host_enable(alp_usb_host_t *h)
{
	/* Gate on the lifecycle byte, not a plain in_use read -- see
	 * alp_slot_claim.h's op_enter/leave doc comment (issue #629). */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->host_enable == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->host_enable(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_usb_host_disable(alp_usb_host_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc = (h->state.ops == NULL || h->state.ops->host_disable == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : h->state.ops->host_disable(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_usb_host_close(alp_usb_host_t *h)
{
	if (h == NULL) return;
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left -- see alp_slot_claim.h (#629).
	 * Idempotent: a second/never-opened close no-ops. */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) return;
	if (h->state.ops != NULL && h->state.ops->host_close != NULL) {
		h->state.ops->host_close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_host(h);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_usb_capabilities(const alp_usb_dev_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
