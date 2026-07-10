/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera class dispatcher.  Owns the public alp_camera_* API
 * surface and routes through the backend registry mechanism
 * shipped in Slice 0 (PR #17).
 *
 * The handle struct layout (struct alp_camera) lives in
 * src/backends/camera/camera_ops.h so per-backend .c files can
 * reach the fields without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/camera/camera_ops.h"

ALP_BACKEND_DEFINE_CLASS(camera);
ALP_BACKEND_ANCHOR(camera);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid
 * pulling in the broader handles.h header (which carries
 * unrelated peripheral pool declarations the dispatcher does
 * not touch). */
#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

static struct alp_camera _pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

static struct alp_camera *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_CAMERA_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_camera *h)
{
	h->in_use = false;
}

alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("camera", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_camera_ops_t *ops = (const alp_camera_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_camera *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_camera_start(alp_camera_t *h)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	return h->state.ops->start(&h->state);
}

alp_status_t alp_camera_stop(alp_camera_t *h)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	return h->state.ops->stop(&h->state);
}

alp_status_t alp_camera_capture(alp_camera_t *h, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	return h->state.ops->capture(&h->state, out, timeout_ms);
}

alp_status_t alp_camera_release(alp_camera_t *h, alp_camera_frame_t *frame)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	if (frame == NULL) {
		return ALP_ERR_INVAL;
	}
	return h->state.ops->release(&h->state, frame);
}

alp_status_t alp_camera_configure_isp(alp_camera_t *h, const alp_camera_isp_config_t *isp)
{
	if (isp == NULL) {
		return ALP_ERR_INVAL;
	}
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	return h->state.ops->configure_isp(&h->state, isp);
}

void alp_camera_close(alp_camera_t *h)
{
	if (h == NULL || !h->in_use) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}

const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
