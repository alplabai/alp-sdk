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
		if (!_in_pool[i].in_use) {
			memset(&_in_pool[i], 0, sizeof(_in_pool[i]));
			_in_pool[i].in_use = true;
			return &_in_pool[i];
		}
	}
	return NULL;
}

static void _free_in(struct alp_audio_in *h)
{
	h->in_use = false;
}

static struct alp_audio_out *_alloc_out(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES; ++i) {
		if (!_out_pool[i].in_use) {
			memset(&_out_pool[i], 0, sizeof(_out_pool[i]));
			_out_pool[i].in_use = true;
			return &_out_pool[i];
		}
	}
	return NULL;
}

static void _free_out(struct alp_audio_out *h)
{
	h->in_use = false;
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
	return h;
}

alp_status_t alp_audio_in_start(alp_audio_in_t *in)
{
	if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
	if (in->state.ops == NULL || in->state.ops->in_start == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return in->state.ops->in_start(&in->state);
}

alp_status_t alp_audio_in_stop(alp_audio_in_t *in)
{
	if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
	if (in->state.ops == NULL || in->state.ops->in_stop == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return in->state.ops->in_stop(&in->state);
}

alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void           *buf,
                               size_t          frames,
                               size_t         *out_frames,
                               uint32_t        timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
	if (buf == NULL || frames == 0) return ALP_ERR_INVAL;
	if (in->state.ops == NULL || in->state.ops->in_read == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return in->state.ops->in_read(&in->state, buf, frames, out_frames, timeout_ms);
}

void alp_audio_in_close(alp_audio_in_t *in)
{
	if (in == NULL || !in->in_use) return;
	if (in->state.ops != NULL && in->state.ops->in_close != NULL) {
		in->state.ops->in_close(&in->state);
	}
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
	return h;
}

alp_status_t alp_audio_out_start(alp_audio_out_t *out)
{
	if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
	if (out->state.ops == NULL || out->state.ops->out_start == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return out->state.ops->out_start(&out->state);
}

alp_status_t alp_audio_out_stop(alp_audio_out_t *out)
{
	if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
	if (out->state.ops == NULL || out->state.ops->out_stop == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return out->state.ops->out_stop(&out->state);
}

alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void      *buf,
                                 size_t           frames,
                                 size_t          *out_frames,
                                 uint32_t         timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
	if (buf == NULL || frames == 0) return ALP_ERR_INVAL;
	if (out->state.ops == NULL || out->state.ops->out_write == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return out->state.ops->out_write(&out->state, buf, frames, out_frames, timeout_ms);
}

alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol)
{
	if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
	if (out->state.ops == NULL || out->state.ops->out_set_volume == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc = out->state.ops->out_set_volume(&out->state, vol);
	if (rc == ALP_OK) out->state.volume = vol;
	return rc;
}

void alp_audio_out_close(alp_audio_out_t *out)
{
	if (out == NULL || !out->in_use) return;
	if (out->state.ops != NULL && out->state.ops->out_close != NULL) {
		out->state.ops->out_close(&out->state);
	}
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
