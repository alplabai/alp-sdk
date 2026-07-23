/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * JPEG-encoder class dispatcher.  Handle layout lives in
 * src/backends/jpeg/jpeg_ops.h.
 */
#include <stddef.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/jpeg.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "alp_z_last_error.h"
#include "backends/jpeg/jpeg_ops.h"

ALP_BACKEND_DEFINE_CLASS(jpeg);
ALP_BACKEND_ANCHOR(jpeg);

#ifndef CONFIG_ALP_SDK_MAX_JPEG_HANDLES
#define CONFIG_ALP_SDK_MAX_JPEG_HANDLES 1
#endif

static struct alp_jpeg _pool[CONFIG_ALP_SDK_MAX_JPEG_HANDLES];

static struct alp_jpeg *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_JPEG_HANDLES; ++i) {
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_jpeg, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_jpeg *h)
{
	alp_slot_release(&h->in_use);
}

alp_jpeg_t *alp_jpeg_open(const alp_jpeg_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("jpeg", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_jpeg_ops_t *ops = (const alp_jpeg_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_jpeg *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend           = be;
	h->state.ops         = ops;
	alp_jpeg_caps_t caps = { 0 };
	alp_status_t    rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_jpeg_encode(alp_jpeg_t                  *h,
                             const alp_jpeg_encode_req_t *req,
                             void                        *out_buf,
                             size_t                       out_cap,
                             size_t                      *out_len)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (req == NULL || out_buf == NULL || out_len == NULL || req->width == 0u ||
	    req->height == 0u) {
		rc = ALP_ERR_INVAL;
	} else {
		rc = h->state.ops->encode(&h->state, req, out_buf, out_cap, out_len);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_jpeg_capabilities(const alp_jpeg_t *h, alp_jpeg_caps_t *out)
{
	if (h == NULL || out == NULL) {
		return ALP_ERR_INVAL;
	}
	*out = h->cached_caps;
	return ALP_OK;
}

void alp_jpeg_close(alp_jpeg_t *h)
{
	if (h == NULL) {
		return;
	}
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(h);
}
