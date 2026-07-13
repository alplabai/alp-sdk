/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * audio class dispatcher.  Owns the public <alp/audio.h> surface --
 * two stateful handle types (alp_audio_in_t, alp_audio_out_t) -- on
 * top of the backend registry mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers both
 * directions because a backend that wires up the active SoC's audio
 * path always implements both (the Zephyr backend lifts the DMIC
 * input AND the alp_i2s_*-delegated output from the legacy
 * src/zephyr/audio_zephyr.c file).  The ops vtable in
 * src/backends/audio/audio_ops.h carries function pointers for both
 * surfaces.
 *
 * Dispatch shape mirrors the security sibling: each open() resolves
 * the backend, allocates from its own static pool, stores the ops
 * pointer on the handle's state struct, and lets I/O ops dispatch
 * through state.ops directly.  Capability getters return per-handle
 * cached snapshots the registry produced at open() time.
 *
 * last_error stamping reuses the existing TLS slot via extern
 * forward decls so the dispatcher does not pull in the broader
 * handles.h header.  Probe() is not invoked for audio -- the v0.5
 * base_caps are sufficient for the two handle surfaces.
 *
 * Pool defaults: 1 in, 1 out (Kconfig-tunable via
 * CONFIG_ALP_SDK_MAX_AUDIO_{IN,OUT}_HANDLES).  Typical E1M-conformant
 * SoMs expose one PDM mic + one I2S DAC; apps that need more raise
 * the cap.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/audio/audio_ops.h"

ALP_BACKEND_DEFINE_CLASS(audio);
/* Pull the audio registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(audio);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES
#define CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES 1
#endif
#ifndef CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES
#define CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES 1
#endif

static struct alp_audio_in  _in_pool[CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES];
static struct alp_audio_out _out_pool[CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES];

static struct alp_audio_in *_alloc_in(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_in_pool[i].in_use)) {
			memset(&_in_pool[i], 0, offsetof(struct alp_audio_in, in_use));
			return &_in_pool[i];
		}
	}
	return NULL;
}

static void _free_in(struct alp_audio_in *h)
{
	alp_slot_release(&h->in_use);
}

static struct alp_audio_out *_alloc_out(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_out_pool[i].in_use)) {
			memset(&_out_pool[i], 0, offsetof(struct alp_audio_out, in_use));
			return &_out_pool[i];
		}
	}
	return NULL;
}

static void _free_out(struct alp_audio_out *h)
{
	alp_slot_release(&h->in_use);
}

/* ================================================================== */
/* Audio input                                                         */
/* ================================================================== */

alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("audio", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_audio_ops_t *ops = (const alp_audio_ops_t *)be->ops;
	if (ops == NULL || ops->in_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_audio_in *h = _alloc_in();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.cfg            = *cfg;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->in_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_in(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN); /* #629 */
	return h;
}

alp_status_t alp_audio_in_start(alp_audio_in_t *in)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_in/_free_in, so mixing it
	 * with a plain read here is a data race, and a racing close could
	 * free the slot mid-op. op_enter counts this op in; begin_close
	 * drains it. #629 */
	if (in == NULL || !alp_handle_op_enter(&in->lifecycle, &in->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (in->state.ops == NULL || in->state.ops->in_start == NULL) {
		alp_handle_op_leave(&in->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = in->state.ops->in_start(&in->state);
	alp_handle_op_leave(&in->active_ops);
	return rc;
}

alp_status_t alp_audio_in_stop(alp_audio_in_t *in)
{
	if (in == NULL || !alp_handle_op_enter(&in->lifecycle, &in->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (in->state.ops == NULL || in->state.ops->in_stop == NULL) {
		alp_handle_op_leave(&in->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = in->state.ops->in_stop(&in->state);
	alp_handle_op_leave(&in->active_ops);
	return rc;
}

alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void           *buf,
                               size_t          frames,
                               size_t         *out_frames,
                               uint32_t        timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	if (buf == NULL || frames == 0) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629): read() can
	 * block up to timeout_ms waiting on frames, so alp_audio_in_close()
	 * drains this op with the sleep-poll alp_handle_begin_close_blocking()
	 * (src/common/alp_slot_claim.c) instead of the busy-spin
	 * alp_handle_begin_close() -- generalised from rpc_dispatch.c's
	 * _rpc_op_enter()/_rpc_begin_close()/_rpc_drain() (GHSA-xhm8). */
	if (in == NULL || !alp_handle_op_enter(&in->lifecycle, &in->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (in->state.ops == NULL || in->state.ops->in_read == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : in->state.ops->in_read(&in->state, buf, frames, out_frames, timeout_ms);
	alp_handle_op_leave(&in->active_ops);
	return rc;
}

void alp_audio_in_close(alp_audio_in_t *in)
{
	if (in == NULL) {
		return;
	}
	/* Sleep-poll drain (issue #629): this pool counts alp_audio_in_read(),
	 * which can block up to its caller's timeout_ms, so
	 * alp_handle_begin_close_blocking() sleeps between polls instead of
	 * busy-spinning -- see src/common/alp_slot_claim.c/.h. Idempotent: a
	 * second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&in->lifecycle, &in->active_ops)) {
		return;
	}
	if (in->state.ops != NULL && in->state.ops->in_close != NULL) {
		in->state.ops->in_close(&in->state);
	}
	alp_lifecycle_set(&in->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_in(in);
}

/* ================================================================== */
/* Audio output                                                        */
/* ================================================================== */

alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("audio", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_audio_ops_t *ops = (const alp_audio_ops_t *)be->ops;
	if (ops == NULL || ops->out_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_audio_out *h = _alloc_out();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.cfg            = *cfg;
	h->state.volume         = 255u; /* unity by default; matches legacy Q8.8 unity */
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->out_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_out(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN); /* #629 */
	return h;
}

alp_status_t alp_audio_out_start(alp_audio_out_t *out)
{
	if (out == NULL || !alp_handle_op_enter(&out->lifecycle, &out->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (out->state.ops == NULL || out->state.ops->out_start == NULL) {
		alp_handle_op_leave(&out->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = out->state.ops->out_start(&out->state);
	alp_handle_op_leave(&out->active_ops);
	return rc;
}

alp_status_t alp_audio_out_stop(alp_audio_out_t *out)
{
	if (out == NULL || !alp_handle_op_enter(&out->lifecycle, &out->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (out->state.ops == NULL || out->state.ops->out_stop == NULL) {
		alp_handle_op_leave(&out->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = out->state.ops->out_stop(&out->state);
	alp_handle_op_leave(&out->active_ops);
	return rc;
}

alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void      *buf,
                                 size_t           frames,
                                 size_t          *out_frames,
                                 uint32_t         timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	if (buf == NULL || frames == 0) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629) -- see
	 * alp_audio_in_read() above for the same rationale. */
	if (out == NULL || !alp_handle_op_enter(&out->lifecycle, &out->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc =
	    (out->state.ops == NULL || out->state.ops->out_write == NULL)
	        ? ALP_ERR_NOT_IMPLEMENTED
	        : out->state.ops->out_write(&out->state, buf, frames, out_frames, timeout_ms);
	alp_handle_op_leave(&out->active_ops);
	return rc;
}

alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol)
{
	if (out == NULL || !alp_handle_op_enter(&out->lifecycle, &out->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (out->state.ops == NULL || out->state.ops->out_set_volume == NULL) {
		alp_handle_op_leave(&out->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = out->state.ops->out_set_volume(&out->state, vol);
	if (rc == ALP_OK) {
		out->state.volume = vol;
	}
	alp_handle_op_leave(&out->active_ops);
	return rc;
}

void alp_audio_out_close(alp_audio_out_t *out)
{
	if (out == NULL) {
		return;
	}
	/* Sleep-poll drain (issue #629): this pool counts alp_audio_out_write(),
	 * which can block up to its caller's timeout_ms, so
	 * alp_handle_begin_close_blocking() sleeps between polls instead of
	 * busy-spinning -- see src/common/alp_slot_claim.c/.h. Idempotent: a
	 * second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&out->lifecycle, &out->active_ops)) {
		return;
	}
	if (out->state.ops != NULL && out->state.ops->out_close != NULL) {
		out->state.ops->out_close(&out->state);
	}
	alp_lifecycle_set(&out->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_out(out);
}

/* ================================================================== */
/* Capability getters                                                  */
/* ================================================================== */

const alp_capabilities_t *alp_audio_in_capabilities(const alp_audio_in_t *in)
{
	return (in != NULL) ? &in->cached_caps : NULL;
}

const alp_capabilities_t *alp_audio_out_capabilities(const alp_audio_out_t *out)
{
	return (out != NULL) ? &out->cached_caps : NULL;
}
