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

#include "alp_slot_claim.h"
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
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_camera, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_camera *h)
{
	alp_slot_release(&h->in_use);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN); /* #629 */
	return h;
}

alp_status_t alp_camera_start(alp_camera_t *h)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc/_free, so mixing it with a
	 * plain read here is a data race, and a racing close could free the
	 * slot mid-op. op_enter counts this op in; begin_close drains it. #629 */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->start(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_camera_stop(alp_camera_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->stop(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_camera_capture(alp_camera_t *h, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	/* Blocking op (caller timeout_ms): lifecycle-only gate, NOT counted via
	 * op_enter -- counting it would make alp_camera_close()'s begin_close
	 * busy-spin until a frame arrives/times out (single-core deadlock when
	 * the closer has >= priority). Same as the ble/wifi/mqtt blocking ops;
	 * residual close-vs-in-flight-capture UAF tracked in the #629 follow-up. */
	if (h == NULL || alp_lifecycle_get(&h->lifecycle) != ALP_HANDLE_LC_OPEN) {
		return ALP_ERR_NOT_READY;
	}
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	return h->state.ops->capture(&h->state, out, timeout_ms);
}

alp_status_t alp_camera_release(alp_camera_t *h, alp_camera_frame_t *frame)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (frame == NULL) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_INVAL;
	}
	alp_status_t rc = h->state.ops->release(&h->state, frame);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_camera_configure_isp(alp_camera_t *h, const alp_camera_isp_config_t *isp)
{
	if (isp == NULL) {
		return ALP_ERR_INVAL;
	}
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->configure_isp(&h->state, isp);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_camera_close(alp_camera_t *h)
{
	if (h == NULL) {
		return;
	}
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left -- so teardown never races an
	 * in-flight op. Idempotent: a second/never-opened close no-ops. #629 */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(h);
}

const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
